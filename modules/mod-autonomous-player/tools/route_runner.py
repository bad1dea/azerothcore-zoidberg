#!/usr/bin/env python3
"""Route-list orchestrator for mod-autonomous-player (Gate 3: levels 1-12).

The module's GuideRuntime executes single quest routes end-to-end
(accept -> walk -> kill/collect until the engine says the objectives
are met -> turn in), idempotently and with bounded failure states
(ADR-048). What it deliberately does NOT do yet is sequence routes:
until this tool existed, a human (or agent) issued each route over SOAP
and re-issued it after bounded failures. This script is that missing
sequencing layer, exactly as HANDOFF.md's Gate 3 gap analysis framed
it: a route-list runner with per-segment bounded retry.

It is an *orchestrator*, not a reimplementation: every in-world
behavior still runs through the module's own commands over the same
SOAP interface live testing has always used (same class of tool as
live_regression_suite.py). The runner only decides WHICH route to
issue next, re-issues after bounded failures (routes are idempotent by
design), and handles cross-route chores the guides don't own yet:
death recovery, selling when bags fill, trainer visits, and
grind-to-level XP filler between authored quests.

Route files are JSON (see routes/): an ordered list of segments, each
one of:
  quest_grind   accept -> walk -> grind until complete -> turn in
                (guidestartquestgrind; multi-objective quests list
                several kill_entries and are re-issued one entry per
                attempt, KNOWN_FAILURES.md #28's authoring lesson)
  quest_accept  walk to the giver and accept (one-shot; for deliver
                quests picked up en route)
  quest_turnin  walk to the turn-in NPC and turn in
  walk          waypoint hops (guidestartmoveto per hop, per-hop retry)
  sell          guidestartselljunk at a vendor (success: grayItems=0)
  train         walk to a class trainer, learn every affordable spell
  grind_to_level  kill+loot cycles (guidestartcombatability) until the
                bot reaches a target level; sells junk when bags fill

State is checkpointed to a JSON file after every segment transition,
and every segment success test reads real state (IsQuestRewarded,
level, grayItems) rather than trusting that a command was sent -- so
killing and restarting the runner (or the worldserver) mid-run resumes
where reality actually is, not where the script thought it was.

Usage:
    python3 route_runner.py --route routes/durotar_orc_warrior_1_12.json \
        --state /tmp/run_state.json [--start-at SEGMENT_ID]

SOAP credentials come from AP_SOAP_HOST/PORT/USER/PASSWORD env vars or
flags, same as live_regression_suite.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live_regression_suite import Config, soap_command  # noqa: E402

POLL_SECONDS = 3.0
# Reconnect budget for a worldserver restart mid-run: SOAP refusals are
# retried this long before the runner gives up entirely.
SOAP_RETRY_BUDGET_SECONDS = 1800.0
# Player::GetQuestStatus values (QuestStatuses.h).
QUEST_STATUS_COMPLETE = 1
QUEST_STATUS_INCOMPLETE = 3


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Runner:
    def __init__(self, cfg: Config, route: dict, state_path: str):
        self.cfg = cfg
        self.route = route
        self.char = route["char"]
        self.account = route["account"]
        self.state_path = state_path
        self.state = {"done": [], "level_history": [], "deaths": 0,
                      "segment_attempts": {}}
        # True while recover_from_death is driving the ghost-walk --
        # wait_guide's own death check must not treat the deliberate
        # ghost state as a fresh death and recurse into recovery.
        self.recovering = False
        if os.path.exists(state_path):
            with open(state_path) as f:
                self.state = json.load(f)

    # ------------------------------------------------------------- SOAP

    def soap(self, cmd: str) -> str:
        """soap_command with a reconnect loop, so a worldserver restart
        mid-run pauses the run instead of killing it."""
        deadline = time.time() + SOAP_RETRY_BUDGET_SECONDS
        delay = 5.0
        while True:
            try:
                return soap_command(self.cfg, cmd, timeout=30.0)
            except Exception as exc:  # connection refused/reset mid-restart
                if time.time() > deadline:
                    raise RuntimeError(
                        f"SOAP unreachable for {SOAP_RETRY_BUDGET_SECONDS}s: {exc}")
                log(f"SOAP error ({exc}); retrying in {delay:.0f}s")
                time.sleep(delay)
                delay = min(delay * 2, 60.0)
                # A restart deregisters every bot; make sure ours is back
                # before re-issuing whatever command failed.
                try:
                    soap_command(self.cfg, ".server info", timeout=10.0)
                    self.ensure_online()
                except Exception:
                    pass

    def ap(self, cmd: str) -> str:
        return self.soap(f".autonomousplayer {cmd}")

    # ------------------------------------------------------ observations

    def ensure_online(self) -> None:
        out = soap_command(self.cfg, ".autonomousplayer status", timeout=15.0)
        if re.search(rf"\b{re.escape(self.char)}\b", out):
            return
        for attempt in range(6):
            out = soap_command(
                self.cfg,
                f".autonomousplayer login {self.account} {self.char}",
                timeout=20.0)
            time.sleep(3.0)
            check = soap_command(self.cfg, ".autonomousplayer status", timeout=15.0)
            if re.search(rf"\b{re.escape(self.char)}\b", check):
                log(f"{self.char} logged in (attempt {attempt + 1})")
                return
            # "already online" right after a kick/restart clears on its own.
            log(f"login attempt {attempt + 1} not registered yet ({out.strip()[:80]}); retrying")
            time.sleep(7.0)
        raise RuntimeError(f"could not get {self.char} online after 6 attempts")

    def bot_status(self) -> dict:
        out = self.ap("status")
        m = re.search(
            rf"{re.escape(self.char)} lvl (\d+) map (\d+) pos \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)"
            rf" hp (\d+)/(\d+) alive=(\w+) combat=(\w+) ghost=(\w+)",
            out)
        if not m:
            return {"online": False}
        status = {
            "online": True,
            "level": int(m.group(1)), "map": int(m.group(2)),
            "x": float(m.group(3)), "y": float(m.group(4)), "z": float(m.group(5)),
            "hp": int(m.group(6)), "max_hp": int(m.group(7)),
            "alive": m.group(8) == "true", "ghost": m.group(10) == "true",
        }
        c = re.search(r"corpse at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", out)
        if c:
            status["corpse"] = (float(c.group(1)), float(c.group(2)), float(c.group(3)))
        return status

    def quest_state(self, quest_id: int) -> dict:
        out = self.ap(f"queststatus {self.char} {quest_id}")
        m = re.search(r"status for '[^']+': (\d+) \(rewarded=(\w+)\) lvl=(\d+) xp=(\d+)", out)
        if not m:
            raise RuntimeError(f"unparseable queststatus: {out.strip()[:200]}")
        return {"status": int(m.group(1)), "rewarded": m.group(2) == "true",
                "level": int(m.group(3)), "xp": int(m.group(4))}

    def level(self) -> int:
        return self.quest_state(788)["level"]  # any quest id works; lvl always printed

    def guide_status(self) -> dict:
        out = self.ap(f"guidestatus {self.char}")
        m = re.search(
            r"step (\d+)/(\d+), action issued=(\w+), finished=(\w+), failed=(\w+)", out)
        if not m:
            return {"parsed": False}
        g = {"parsed": True, "step": int(m.group(1)), "steps": int(m.group(2)),
             "finished": m.group(4) == "true", "failed": m.group(5) == "true"}
        inv = re.search(r"turnInEngineRefused=(\w+) grayItems=(\d+) freeBagSlots=(\d+)", out)
        if inv:
            g["turn_in_refused"] = inv.group(1) == "true"
            g["gray_items"] = int(inv.group(2))
            g["free_bag_slots"] = int(inv.group(3))
        return g

    # -------------------------------------------------------- recovery

    def recover_from_death(self) -> None:
        """Release, ghost-walk back to the corpse, reclaim, verify alive."""
        if self.recovering:
            return  # already mid-recovery; never recurse
        self.recovering = True
        try:
            self._recover_from_death()
        finally:
            self.recovering = False

    def _recover_from_death(self) -> None:
        self.state["deaths"] += 1
        self.save_state()
        log(f"death #{self.state['deaths']} -- starting recovery")
        released_at = time.time()
        self.ap(f"releasespirit {self.char}")
        time.sleep(3.0)
        st = self.bot_status()
        corpse = st.get("corpse")
        if corpse:
            # Ghost-walk back with the same bisecting walker every
            # other movement uses -- the graveyard can be several
            # hundred yards out, far beyond a single MoveTo's silent
            # path-length limit.
            if not self.walk_toward(corpse[0], corpse[1], corpse[2],
                                    arrive_within=25.0, max_issues=30,
                                    allow_ghost=True):
                log("ghost walk stalled; attempting reclaim from here")
        # Engine requires ~30s since release and <39yd to the corpse.
        wait_left = 31.0 - (time.time() - released_at)
        if wait_left > 0:
            time.sleep(wait_left)
        for _ in range(10):
            self.ap(f"reclaimcorpse {self.char}")
            time.sleep(5.0)
            st = self.bot_status()
            if st.get("alive") and not st.get("ghost"):
                log("recovered: alive again")
                self.recovery_failures = 0
                time.sleep(10.0)  # let health tick up a little before fighting
                return
            time.sleep(10.0)
        # Not fatal on its own: the caller's next death check re-enters
        # recovery (fresh corpse read, fresh ghost-walk). Only give up
        # for real after several full recovery cycles fail in a row.
        self.recovery_failures = getattr(self, "recovery_failures", 0) + 1
        log(f"death recovery attempt failed (cycle {self.recovery_failures}/5)")
        if self.recovery_failures >= 5:
            raise RuntimeError("death recovery failed 5 full cycles -- needs intervention")

    def check_alive_or_recover(self) -> bool:
        """Returns True if a death was handled (caller should re-issue)."""
        st = self.bot_status()
        if not st.get("online"):
            self.ensure_online()
            return True
        if st.get("ghost") or not st.get("alive"):
            self.recover_from_death()
            return True
        return False

    def unstick(self, seg: dict) -> bool:
        """The /stuck equivalent: teleport to the segment's authored
        hub tele-point after walking has genuinely given up (real
        stranding observed live: chasing quest mobs up a ridge put the
        bot on a mesa whose polys don't path back down). Used at most
        once per 120s so a genuinely broken segment still fails loudly
        instead of teleport-looping. Counted and reported honestly."""
        point = seg.get("unstick") or self.route.get("unstick")
        if not point:
            return False
        now = time.time()
        if now - getattr(self, "_last_unstick", 0.0) < 120.0:
            return False
        self._last_unstick = now
        self.state["unsticks"] = self.state.get("unsticks", 0) + 1
        self.save_state()
        log(f"UNSTICK #{self.state['unsticks']}: teleporting to {point}")
        self.soap(f".tele name {self.char} {point}")
        time.sleep(3.0)
        return True

    # ------------------------------------------------------ guide waits

    def walk_toward(self, x: float, y: float, z: float,
                    arrive_within: float = 25.0, max_issues: int = 24,
                    allow_ghost: bool = False) -> bool:
        """Re-issue guidestartmoveto until the bot is within range.

        A single MoveTo guide is bounded by MaxOperationTicks (~20 real
        seconds, roughly 140yd of walking), so any longer leg fails
        bounded and must be re-issued -- progress accumulates across
        re-issues because each one starts from wherever the bot
        actually is. Gives up after two consecutive issues with no real
        progress (a genuinely unreachable target, not just a long walk).
        """
        no_progress = 0
        for _ in range(max_issues):
            st = self.bot_status()
            if not st.get("online"):
                return False
            if (st.get("ghost") or not st.get("alive")) and not allow_ghost:
                return False  # caller's death/online handling takes over
            before = (st["x"], st["y"])
            dist = ((before[0] - x) ** 2 + (before[1] - y) ** 2) ** 0.5
            if dist <= arrive_within:
                return True
            # MoveTo silently refuses long paths (observed live: ~130yd
            # legs walk, ~250yd legs produce ZERO movement -- the
            # navmesh path budget runs out and, since the straight-line
            # NOPATH fallback was removed with the flight fix, the bot
            # correctly does nothing). On no-progress, bisect the leg:
            # aim at the midpoint (then quarter-point) between the bot
            # and the target until movement resumes.
            frac = 1.0 / (2 ** no_progress)
            lx = before[0] + (x - before[0]) * frac
            ly = before[1] + (y - before[1]) * frac
            lz = st["z"] + (z - st["z"]) * frac
            self.issue_and_wait(
                f"guidestartmoveto {self.char} {lx:.1f} {ly:.1f} {lz:.1f}", 120)
            st = self.bot_status()
            moved = ((st.get("x", before[0]) - before[0]) ** 2 +
                     (st.get("y", before[1]) - before[1]) ** 2) ** 0.5
            if moved < 10.0:
                no_progress += 1
                if no_progress >= 4:
                    log(f"walk_toward ({x:.0f},{y:.0f}): no progress even at "
                        f"1/8 leg ({dist:.0f}yd away) -- giving up")
                    return False
            else:
                no_progress = 0
        st = self.bot_status()
        dist = ((st.get("x", 1e9) - x) ** 2 + (st.get("y", 1e9) - y) ** 2) ** 0.5
        return dist <= arrive_within

    def wait_guide(self, wall_timeout: float) -> str:
        """Poll until the current guide finishes/fails, handling death.
        Returns 'finished' | 'failed' | 'died' | 'timeout'."""
        deadline = time.time() + wall_timeout
        ticks = 0
        while time.time() < deadline:
            time.sleep(POLL_SECONDS)
            ticks += 1
            g = self.guide_status()
            if g.get("finished"):
                return "failed" if g.get("failed") else "finished"
            if ticks % 5 == 0 and not self.recovering:  # watch for death mid-guide
                st = self.bot_status()
                if not st.get("online"):
                    self.ensure_online()
                    return "died"  # session recycled; treat like a retry
                if st.get("ghost") or not st.get("alive"):
                    self.recover_from_death()
                    return "died"
        return "timeout"

    def issue_and_wait(self, command: str, wall_timeout: float) -> str:
        self.ap(command)
        return self.wait_guide(wall_timeout)

    # ---------------------------------------------------- segment types

    def seg_quest_grind(self, seg: dict) -> bool:
        q = seg["quest"]
        if self.quest_state(q)["rewarded"]:
            log(f"quest {q} already rewarded; skipping")
            return True
        kill_entries = seg.get("kill_entries") or [
            {"entry": seg["kill_entry"], "x": seg["x"], "y": seg["y"], "z": seg["z"]}]
        # Attempts that made real progress (XP moved: kills happened, or
        # the quest advanced) do not count against the stall budget --
        # a grind naturally oscillates between killing and re-walking
        # (kill wanderers, drift off the waypoint, local drought, fail
        # bounded, re-issue), and that oscillation is progress, not a
        # stall. Only a no-XP, no-state-change attempt counts.
        stall_budget = seg.get("attempts", 10)
        stalls = 0
        attempt = 0
        last_xp = self.quest_state(q)["xp"]
        while stalls < stall_budget:
            attempt += 1
            qs = self.quest_state(q)
            if qs["rewarded"]:
                return True
            if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                # Not accepted yet -- the guide's AcceptQuest step only
                # searches 100yd, so get near the giver first. An NPC
                # under an overhang (e.g. the Den burrow) must be
                # approached VIA a ground-level point in front of it:
                # walking straight at its coordinates from the wrong
                # side "arrives" (2D) on the terrain layer above it.
                via = seg.get("giver_via")
                if via:
                    self.walk_toward(via[0], via[1], via[2], arrive_within=15.0)
                self.walk_toward(seg["giver_x"], seg["giver_y"], seg["giver_z"],
                                 arrive_within=60.0)
            if qs["status"] == QUEST_STATUS_COMPLETE:
                # Objectives done -- put the waypoint at the turn-in NPC
                # and pre-walk there, so the walk-back happens even when
                # the grind field is beyond TurnInQuest's 150yd search
                # radius (KNOWN_FAILURES.md #28).
                via = seg.get("turnin_via")
                if via:
                    if not self.walk_toward(via[0], via[1], via[2], arrive_within=15.0):
                        self.unstick(seg)
                        self.walk_toward(via[0], via[1], via[2], arrive_within=15.0)
                    wp = (via[0], via[1], via[2])
                else:
                    wp = (seg["turnin_x"], seg["turnin_y"], seg["turnin_z"])
                    if not self.walk_toward(wp[0], wp[1], wp[2], arrive_within=60.0):
                        self.unstick(seg)
                        self.walk_toward(wp[0], wp[1], wp[2], arrive_within=60.0)
                ke = kill_entries[0]
            else:
                ke = kill_entries[(attempt - 1) % len(kill_entries)]
                wp = (ke["x"], ke["y"], ke["z"])
                # Leaving a via-NPC's pocket needs the same ground-level
                # detour as approaching it (#30 works both ways: paths
                # OUT of the burrow toward the field strand the bot on
                # the hill layer just like paths in).
                via = seg.get("giver_via")
                if via:
                    self.walk_toward(via[0], via[1], via[2], arrive_within=15.0)
                # Pre-walk with the runner's own re-issuing loop: a
                # guide's single MoveTo leg is bounded (~20s, roughly
                # 140yd), so any longer approach must be walked HERE,
                # not inside the guide.
                if not self.walk_toward(wp[0], wp[1], wp[2], arrive_within=40.0):
                    self.unstick(seg)
            result = self.issue_and_wait(
                f"guidestartquestgrind {self.char} {q} {seg['giver']} {ke['entry']} "
                f"{seg['turnin']} {seg.get('choice', 0)} {wp[0]:.1f} {wp[1]:.1f} {wp[2]:.1f}",
                seg.get("wall_timeout", 900))
            qs_after = self.quest_state(q)
            progressed = (qs_after["xp"] != last_xp or qs_after["level"] > qs["level"]
                          or qs_after["status"] != qs["status"] or qs_after["rewarded"])
            last_xp = qs_after["xp"]
            stalls = 0 if progressed else stalls + 1
            log(f"quest {q} attempt {attempt} (entry {ke['entry']}): {result}"
                f" (stalls {stalls}/{stall_budget})")
            if qs_after["rewarded"]:
                return True
            g = self.guide_status()
            if g.get("turn_in_refused") or g.get("free_bag_slots", 99) == 0:
                log(f"quest {q}: bags full / turn-in refused -- selling junk first")
                self.seg_sell(seg.get("vendor", self.route["home_vendor"]))
        return self.quest_state(q)["rewarded"]

    def seg_quest_accept(self, seg: dict) -> bool:
        q = seg["quest"]
        qs = self.quest_state(q)
        if qs["rewarded"] or qs["status"] in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
            return True
        for attempt in range(seg.get("attempts", 4)):
            via = seg.get("via")
            if via:
                self.walk_toward(via[0], via[1], via[2], arrive_within=15.0)
            self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=3.0)
            self.ap(f"acceptquest {self.char} {q} {seg['giver']}")
            time.sleep(2.0)
            qs = self.quest_state(q)
            if qs["status"] in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                return True
            log(f"accept quest {q} attempt {attempt + 1}: status={qs['status']}")
            if self.check_alive_or_recover():
                continue
        return False

    def seg_quest_turnin(self, seg: dict) -> bool:
        q = seg["quest"]
        if self.quest_state(q)["rewarded"]:
            return True
        for attempt in range(seg.get("attempts", 4)):
            via = seg.get("via")
            if via:
                self.walk_toward(via[0], via[1], via[2], arrive_within=15.0)
            self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=3.0)
            self.ap(f"turnin {self.char} {q} {seg['turnin']} {seg.get('choice', 0)}")
            time.sleep(2.0)
            if self.quest_state(q)["rewarded"]:
                return True
            log(f"turnin quest {q} attempt {attempt + 1}: not rewarded yet")
            if self.check_alive_or_recover():
                continue
        return self.quest_state(q)["rewarded"]

    def seg_walk(self, seg: dict) -> bool:
        for i, hop in enumerate(seg["hops"]):
            if not self.walk_toward(hop[0], hop[1], hop[2]):
                if self.check_alive_or_recover() or self.unstick(seg):
                    if self.walk_toward(hop[0], hop[1], hop[2]):
                        continue
                log(f"walk hop {i + 1}/{len(seg['hops'])} unreachable")
                return False
        return True

    def seg_sell(self, seg: dict) -> bool:
        self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=60.0)
        for attempt in range(seg.get("attempts", 3)):
            result = self.issue_and_wait(
                f"guidestartselljunk {self.char} {seg['vendor']} "
                f"{seg['x']:.1f} {seg['y']:.1f} {seg['z']:.1f}", 300)
            g = self.guide_status()
            if result == "finished" or g.get("gray_items", 1) == 0:
                return True
            log(f"sell attempt {attempt + 1}: {result}, grayItems={g.get('gray_items')}")
        return False

    def seg_train(self, seg: dict) -> bool:
        self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=10.0)
        learned = 0
        for _ in range(seg.get("max_spells", 30)):
            out = self.ap(f"learnspell {self.char} {seg['trainer']}")
            if "no spell" in out:
                break
            m = re.search(r"has spell after=(\w+)", out)
            if m and m.group(1) == "true":
                learned += 1
                time.sleep(1.0)
            else:
                # Couldn't actually learn (usually not enough money) --
                # stop rather than loop on the same rejection.
                log(f"train: stopping, spell not learned ({out.strip()[-90:]})")
                break
        log(f"train: learned {learned} spell(s)")
        return True  # training is best-effort; money-gated by design

    def seg_grind_to_level(self, seg: dict) -> bool:
        target = seg["level"]
        consecutive_failures = 0
        deadline = time.time() + seg.get("max_minutes", 240) * 60
        while time.time() < deadline:
            lvl = self.level()
            if lvl >= target:
                log(f"grind_to_level {target}: reached (level {lvl})")
                return True
            g = self.guide_status()
            if g.get("free_bag_slots", 99) <= 2 and "vendor" in seg:
                self.seg_sell(seg["vendor"])
            st = self.bot_status()
            dist2 = (st.get("x", 1e9) - seg["x"]) ** 2 + (st.get("y", 1e9) - seg["y"]) ** 2
            if dist2 > 80.0 ** 2:
                if not self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=40.0):
                    self.unstick(seg)
            spell = seg.get("spell", self.route.get("opportunistic_spell", 0))
            result = self.issue_and_wait(
                f"guidestartcombatability {self.char} {seg['entry']} {spell}",
                seg.get("cycle_timeout", 150))
            if result == "finished":
                consecutive_failures = 0
            else:
                consecutive_failures += 1
                log(f"grind cycle: {result} ({consecutive_failures} consecutive)")
                if consecutive_failures >= 8:
                    # Selection droughts (killed-out camp) usually clear
                    # with respawns; a wedged session does not. Recycle
                    # the session (#23/#24) and keep going.
                    log("grind: 6 consecutive failures -- recycling bot session")
                    self.ap(f"logout {self.char}")
                    time.sleep(5.0)
                    self.ensure_online()
                    consecutive_failures = 0
            self.record_level()
        log(f"grind_to_level {target}: max_minutes budget exhausted")
        return self.level() >= target

    # ----------------------------------------------------------- driver

    def record_level(self) -> None:
        qs = self.quest_state(788)
        hist = self.state["level_history"]
        if not hist or hist[-1]["level"] != qs["level"]:
            hist.append({"level": qs["level"], "xp": qs["xp"], "t": time.strftime("%F %T")})
            self.save_state()
            log(f"*** LEVEL {qs['level']} ***")

    def save_state(self) -> None:
        with open(self.state_path, "w") as f:
            json.dump(self.state, f, indent=1)

    def run(self, start_at: str | None = None) -> int:
        handlers = {
            "quest_grind": self.seg_quest_grind,
            "quest_accept": self.seg_quest_accept,
            "quest_turnin": self.seg_quest_turnin,
            "walk": self.seg_walk,
            "sell": self.seg_sell,
            "train": self.seg_train,
            "grind_to_level": self.seg_grind_to_level,
        }
        self.ensure_online()
        self.record_level()
        started = start_at is None
        for seg in self.route["segments"]:
            sid = seg["id"]
            if not started:
                started = sid == start_at
                if not started:
                    continue
            if sid in self.state["done"]:
                continue
            req = seg.get("requires_quest")
            if req and not self.quest_state(req)["rewarded"]:
                log(f"[{sid}] prerequisite quest {req} not rewarded -- skipping")
                continue
            min_lvl = seg.get("min_level")
            if min_lvl and self.level() < min_lvl:
                log(f"[{sid}] below min_level {min_lvl} -- skipping (grind segments gate levels)")
                continue
            log(f"=== segment [{sid}] ({seg['type']}) ===")
            self.check_alive_or_recover()
            ok = handlers[seg["type"]](seg)
            self.record_level()
            if ok:
                self.state["done"].append(sid)
                self.save_state()
                log(f"[{sid}] DONE (level {self.level()})")
            elif seg.get("optional"):
                log(f"[{sid}] FAILED but optional -- continuing")
                self.state["done"].append(sid)
                self.save_state()
            else:
                log(f"[{sid}] FAILED (required) -- stopping for intervention")
                return 1
        log(f"route complete at level {self.level()}; deaths={self.state['deaths']}")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--route", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--start-at", default=None)
    parser.add_argument("--host", default=os.environ.get("AP_SOAP_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("AP_SOAP_PORT", "7878")))
    parser.add_argument("--user", default=os.environ.get("AP_SOAP_USER", ""))
    parser.add_argument("--password", default=os.environ.get("AP_SOAP_PASSWORD", ""))
    args = parser.parse_args()
    if not args.user or not args.password:
        print("missing SOAP credentials", file=sys.stderr)
        return 2
    with open(args.route) as f:
        route = json.load(f)
    cfg = Config(host=args.host, port=args.port, user=args.user,
                 password=args.password, bot_account=route["account"],
                 bot_char=route["char"], creature_entry=0)
    return Runner(cfg, route, args.state).run(args.start_at)


if __name__ == "__main__":
    sys.exit(main())
