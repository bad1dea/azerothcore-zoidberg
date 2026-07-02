#!/usr/bin/env python3
"""Live regression suite for mod-autonomous-player (Gate 3, review priority 7).

Every "Verified live" claim in docs/autonomous-player/*.md up to this
point was a one-off manual observation over the worldserver's SOAP
console interface -- there was no automated regression protection at
all. This script runs a small, real set of assertions against a live
deployed worldserver over that same SOAP interface, so a future change
that breaks something already proven working fails loudly and
immediately instead of waiting for a human (or agent) to notice by eye.

It is deliberately NOT a gtest/BUILD_TESTING unit-test suite -- this
repo's module build path doesn't wire that up (see TEST_MATRIX.md), and
most of what this project needs to protect (real opcode handlers, real
navmesh movement, real DB state) only exists meaningfully against a
live, deployed server anyway. This is the same class of tool as
check_no_playerbots_dependency.sh / check_no_forbidden_apis.sh: a cheap,
real, automatable check that catches a real class of regression, not an
attempt to replace careful live verification for new features.

Concretely, this suite would have caught this session's own
IsHostileTo -> IsValidAttackTarget regression automatically (see
ARCHITECTURE.md ADR-031): TEST_ATTACKABLE below asserts exactly that.

Usage:
    python3 live_regression_suite.py --host zoidberg.example --user SOAPADMIN \
        --password '...' --bot-account ap_test1 --bot-char Grunttestbot \
        --creature-entry 3098

Or set AP_SOAP_HOST / AP_SOAP_PORT / AP_SOAP_USER / AP_SOAP_PASSWORD /
AP_SOAP_BOT_ACCOUNT / AP_SOAP_BOT_CHAR / AP_SOAP_CREATURE_ENTRY env vars
instead of flags (credentials are deliberately never hardcoded here or
checked in anywhere -- see the SOAP access reference in agent memory for
how to obtain them for a given deployment).

Exit code 0 if every test passes, 1 otherwise. Prints a PASS/FAIL line
per test plus a final summary.
"""

from __future__ import annotations

import argparse
import http.client
import os
import re
import sys
import time
import xml.sax.saxutils as saxutils
from dataclasses import dataclass


@dataclass
class Config:
    host: str
    port: int
    user: str
    password: str
    bot_account: str
    bot_char: str
    creature_entry: int


def soap_command(config: Config, command: str, timeout: float = 15.0) -> str:
    """Sends one console command over SOAP, returns the <result> text.

    Uses http.client directly (stdlib only, no extra dependency) against
    the worldserver's built-in SOAP endpoint -- the same mechanism every
    "Verified live" claim in this project's docs was produced with (see
    the autonomous-player-zoidberg-soap-access memory entry for the
    history of how this was (re)discovered).
    """
    escaped = saxutils.escape(command)
    body = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<SOAP-ENV:Envelope xmlns:SOAP-ENV="http://schemas.xmlsoap.org/soap/envelope/">'
        "<SOAP-ENV:Body>"
        '<ns1:executeCommand xmlns:ns1="urn:AC">'
        f"<command>{escaped}</command>"
        "</ns1:executeCommand>"
        "</SOAP-ENV:Body></SOAP-ENV:Envelope>"
    )

    import base64

    auth = base64.b64encode(f"{config.user}:{config.password}".encode()).decode()
    conn = http.client.HTTPConnection(config.host, config.port, timeout=timeout)
    try:
        conn.request(
            "POST",
            "/",
            body=body,
            headers={
                "Content-Type": "text/xml",
                "Authorization": f"Basic {auth}",
            },
        )
        response = conn.getresponse()
        raw = response.read().decode("utf-8", errors="replace")
    finally:
        conn.close()

    match = re.search(r"<result>(.*?)</result>", raw, re.S)
    if match:
        return match.group(1).replace("&#xD;", "").strip()

    fault = re.search(r"<faultstring>(.*?)</faultstring>", raw, re.S)
    if fault:
        raise RuntimeError(f"SOAP fault for command {command!r}: {fault.group(1).strip()}")

    raise RuntimeError(f"Unrecognized SOAP response for command {command!r}: {raw!r}")


def parse_kv(text: str) -> dict[str, str]:
    """Parses the `key=value` tokens this module's debug commands print
    (e.g. `alive=true evading=false attackable=true` or, comma-separated,
    `finished=true, failed=false`) into a dict. Good enough for this
    suite's assertions -- not a general parser. Values stop at whitespace
    OR a trailing comma -- `guidestatus`'s comma-separated fields would
    otherwise parse `finished=true,` (with the comma) as the value,
    which never equals the literal string `"true"` a test compares
    against (found by actually running this suite, not by inspection)."""
    return dict(re.findall(r"(\w+)=([^,\s]+)", text))


class TestFailure(Exception):
    pass


def wait_for_guide_finish(config: Config, char_name: str, timeout_s: float = 40.0) -> dict[str, str]:
    deadline = time.monotonic() + timeout_s
    last_status = ""
    while time.monotonic() < deadline:
        last_status = soap_command(config, f".autonomousplayer guidestatus {char_name}")
        fields = parse_kv(last_status)
        if fields.get("finished") == "true":
            return fields
        time.sleep(2.0)
    raise TestFailure(f"guide never reached finished=true within {timeout_s}s (last status: {last_status!r})")


def test_soap_connectivity(config: Config) -> None:
    result = soap_command(config, "server info")
    if "AzerothCore rev." not in result:
        raise TestFailure(f"unexpected 'server info' output: {result!r}")


def test_bot_login_and_status(config: Config) -> None:
    soap_command(config, f".autonomousplayer login {config.bot_account} {config.bot_char}")
    deadline = time.monotonic() + 15.0
    last_status = ""
    while time.monotonic() < deadline:
        last_status = soap_command(config, f".autonomousplayer status {config.bot_char}")
        if config.bot_char in last_status and "alive=true" in last_status:
            return
        time.sleep(2.0)
    raise TestFailure(f"'{config.bot_char}' never reached a live alive=true status: {last_status!r}")


def parse_low_guid(text: str) -> str | None:
    """Extracts the `Low: N` component from an `ObjectGuid::ToString()`
    blob (the format every creature/pet debug command in this module
    prints). Good enough to compare "is this the same object" across two
    separate command outputs without a full GUID parser."""
    match = re.search(r"Low:\s*(\d+)", text)
    return match.group(1) if match else None


def test_creature_attackable_not_merely_hostile(config: Config) -> None:
    """Regression test for ARCHITECTURE.md ADR-031: a first version of
    IsSafeToEngage used IsHostileTo and would have rejected ordinary
    faction-neutral questing wildlife (e.g. Mottled Boar) entirely. This
    asserts the real, correct semantics directly against a live target so
    that specific regression can never silently reappear.

    Real interaction found live while first adding pets (ADR-037): if
    `config.bot_char` has tamed a pet of the same species as
    `config.creature_entry` (e.g. a Hunter with a tamed Mottled Boar,
    testing against entry 3098), `targetsafety`'s nearest-match search
    can find the bot's *own pet* instead of a wild one -- and
    `attackable=false` for your own pet is the CORRECT answer
    (`IsValidAttackTarget` rightly excludes it), not a regression. Skip
    cleanly if the found guid matches the bot's own pet rather than
    asserting on it."""
    deadline = time.monotonic() + 30.0
    last_result = ""
    while time.monotonic() < deadline:
        last_result = soap_command(
            config, f".autonomousplayer targetsafety {config.bot_char} {config.creature_entry} 300"
        )
        if "No creature with entry" not in last_result:
            break
        time.sleep(2.0)
    else:
        raise TestFailure(
            f"no live creature of entry {config.creature_entry} found within 300 yards "
            f"of '{config.bot_char}' to test against: {last_result!r}"
        )

    found_guid = parse_low_guid(last_result)
    pet_result = soap_command(config, f".autonomousplayer petstatus {config.bot_char}")
    pet_guid = parse_low_guid(pet_result) if "has no pet" not in pet_result else None
    if found_guid is not None and found_guid == pet_guid:
        return

    fields = parse_kv(last_result)
    if fields.get("alive") != "true":
        # Found a dead one first; not this test's concern (real assertion
        # is about the attackable check specifically) -- skip cleanly
        # rather than fail on an unrelated corpse.
        return
    if fields.get("attackable") != "true":
        raise TestFailure(
            f"expected attackable=true for a live entry-{config.creature_entry} creature, "
            f"got: {last_result!r} (this is exactly the IsHostileTo regression ADR-031 found)"
        )


def test_guidestartcombat_completes_cleanly(config: Config) -> None:
    """Regression test for the core KillNearest loop (ADR-023/028/030/031
    combined): walk to + kill + loot a real creature fully automatically,
    with no manual command after the trigger, must finish cleanly.

    Deliberately does NOT hard-require `lastLootVerified=true` -- ADR-030
    documents that as legitimately best-effort (e.g. full/near-full bags
    genuinely reject an item, and the guide correctly proceeds anyway
    rather than getting stuck). Found live while first running this
    suite: a long-lived test bot with 18 inventory items produced a real
    `lastLootAttempted=true, lastLootVerified=false` on an otherwise
    clean run -- asserting `lastLootVerified=true` here would make this
    suite flake on a real, working-as-designed state instead of an
    actual regression. `finished=true, failed=false` is the real
    contract; loot verification is reported, not enforced."""
    soap_command(config, f".autonomousplayer guidestartcombat {config.bot_char} {config.creature_entry}")
    fields = wait_for_guide_finish(config, config.bot_char)
    if fields.get("failed") != "false":
        raise TestFailure(f"guidestartcombat did not complete cleanly: {fields}")
    if fields.get("lastLootVerified") != "true":
        print(
            f"      note: lastLootVerified=false this run (attempted={fields.get('lastLootAttempted')}) "
            "-- not a failure, see ADR-030; likely a near-full-bags bot"
        )


def parse_position(text: str) -> tuple[float, float, float] | None:
    """Extracts `pos (x, y, z)` from `.autonomousplayer status` output."""
    match = re.search(r"pos \(([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)\)", text)
    if not match:
        return None
    return (float(match.group(1)), float(match.group(2)), float(match.group(3)))


def test_guidestartmoveto_unreachable_target_times_out(config: Config) -> None:
    """Regression test for ADR-028's bounded-wait guarantee: a guide step
    that can never complete (here, an intentionally absurd coordinate)
    must fail with failed=true within the documented bound, not hang
    forever.

    Restores the bot to its pre-test position afterward (KNOWN_FAILURES.md
    #14): this test deliberately walks the bot ~20-45 real seconds toward
    a literal (5000, 5000, 500) every time it runs, which is real,
    unbounded, cumulative drift across repeated runs -- confirmed this
    project's own session logs show a real test character drifting well
    over 1500 yards from its starting area purely from this test being
    run many times. The walk itself was directly investigated and not
    found to cause terrain clipping (see KNOWN_FAILURES.md #14's full
    writeup), but leaving a real character stranded wherever a bounded,
    intentionally-doomed walk happens to stop is still bad practice this
    test can cheaply avoid."""
    pre_status = soap_command(config, f".autonomousplayer status {config.bot_char}")
    pre_position = parse_position(pre_status)

    soap_command(config, f".autonomousplayer guidestartmoveto {config.bot_char} 5000 5000 500")
    # MaxOperationTicks (45) at the observed-variable real tick rate
    # (ADR-028: ~2.3 ticks/s under light load, but slower when the
    # server is busier) can take up to ~45s in practice -- give real
    # margin above that rather than a tight bound that flakes under
    # load.
    fields = wait_for_guide_finish(config, config.bot_char, timeout_s=70.0)

    if pre_position is not None:
        x, y, z = pre_position
        soap_command(config, f".autonomousplayer guidestartmoveto {config.bot_char} {x} {y} {z}")
        try:
            wait_for_guide_finish(config, config.bot_char, timeout_s=70.0)
        except TestFailure:
            # Best-effort: a real path back may not be findable either
            # (e.g. the pre-test position was itself already dubious) --
            # this is cleanup, not the test's own assertion, so a failure
            # here should not mask the real result below.
            print(f"      note: could not walk back to pre-test position {pre_position}, leaving bot where it stopped")

    if fields.get("failed") != "true":
        raise TestFailure(
            f"expected an unreachable guidestartmoveto to fail with failed=true, got: {fields}"
        )


TESTS = [
    ("soap_connectivity", test_soap_connectivity),
    ("bot_login_and_status", test_bot_login_and_status),
    ("creature_attackable_not_merely_hostile", test_creature_attackable_not_merely_hostile),
    ("guidestartcombat_completes_cleanly", test_guidestartcombat_completes_cleanly),
    ("guidestartmoveto_unreachable_target_times_out", test_guidestartmoveto_unreachable_target_times_out),
]


def parse_args() -> Config:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("AP_SOAP_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("AP_SOAP_PORT", "7878")))
    parser.add_argument("--user", default=os.environ.get("AP_SOAP_USER", ""))
    parser.add_argument("--password", default=os.environ.get("AP_SOAP_PASSWORD", ""))
    parser.add_argument("--bot-account", default=os.environ.get("AP_SOAP_BOT_ACCOUNT", ""))
    parser.add_argument("--bot-char", default=os.environ.get("AP_SOAP_BOT_CHAR", ""))
    parser.add_argument(
        "--creature-entry", type=int, default=int(os.environ.get("AP_SOAP_CREATURE_ENTRY", "3098"))
    )
    args = parser.parse_args()

    missing = [
        name
        for name, value in (
            ("--user/AP_SOAP_USER", args.user),
            ("--password/AP_SOAP_PASSWORD", args.password),
            ("--bot-account/AP_SOAP_BOT_ACCOUNT", args.bot_account),
            ("--bot-char/AP_SOAP_BOT_CHAR", args.bot_char),
        )
        if not value
    ]
    if missing:
        parser.error(f"missing required value(s): {', '.join(missing)}")

    return Config(
        host=args.host,
        port=args.port,
        user=args.user,
        password=args.password,
        bot_account=args.bot_account,
        bot_char=args.bot_char,
        creature_entry=args.creature_entry,
    )


def main() -> int:
    config = parse_args()

    failures = 0
    for name, test_fn in TESTS:
        try:
            test_fn(config)
        except TestFailure as exc:
            print(f"FAIL  {name}: {exc}")
            failures += 1
        except Exception as exc:  # noqa: BLE001 - report unexpected errors as failures too
            print(f"ERROR {name}: {exc!r}")
            failures += 1
        else:
            print(f"PASS  {name}")

    print(f"\n{len(TESTS) - failures}/{len(TESTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
