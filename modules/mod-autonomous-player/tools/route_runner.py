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
import math
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from live_regression_suite import Config, soap_command  # noqa: E402

POLL_SECONDS = 3.0
# Emergency-only teleport escape hatch; THE RULE is bots walk.
ALLOW_TELE = False
# Hub name -> coordinates (mirrors the game_tele rows; used for WALKED
# unsticks now that teleporting is rule-barred).
HUBS = {
    "RRVotFloor": (-610.1, -4253.5, 39.0), "RRScorpidField": (-378.2, -4125.5, 50.8),
    "RRSenjin": (-825.6, -4920.8, 19.7), "RRRazorHill": (287.3, -4724.9, 13.2),
    "RRUkor": (-599.4, -4715.3, 35.2), "RRTiragarde": (-65.0, -4961.5, 21.5),
    "RRDustwind": (952.0, -4754.9, 23.8), "RRMargoz": (1102.1, -4945.4, 15.7),
    "RRSkullRock": (1493.0, -4762.4, 5.9), "APCampNarache": (-2912.7, -257.5, 53.0),
    "APFamiliarCamp": (-152.7, -4264.4, 61.5), "APFamiliarTriple": (-40.0, -4227.0, 64.5),
    "APBoarCluster": (-680.9, -4284.8, 40.0), "APDenKaltunk": (-600.1, -4186.2, 41.3),
    "MGBloodhoof": (-2340.0, -400.0, -8.0), "MGRaintotem": (-1150.4, -1027.4, 3.6),
    "DKDeathknell": (1843.3, 1639.9, 97.8), "DKBrill": (2269.5, 244.9, 34.3),
    "BESunstrider": (10352.0, -6359.9, 34.1), "BEFalconwing": (9476.9, -6859.2, 17.4),
    "ELWNorthshire": (-8913.0, -184.0, 81.0), "ELWGoldshire": (-9464.0, 62.0, 56.5),
    "DMColdridge": (-6236.7, 331.1, 382.9), "DMKharanos": (-5602.0, -510.0, 398.0),
    "BRCrossroads": (-450.0, -2600.0, 96.0),
}
# Reconnect budget for a worldserver restart mid-run: SOAP refusals are
# retried this long before the runner gives up entirely.
SOAP_RETRY_BUDGET_SECONDS = 1800.0
# Player::GetQuestStatus values (QuestStatuses.h).
QUEST_STATUS_NONE = 0
QUEST_STATUS_COMPLETE = 1
QUEST_STATUS_INCOMPLETE = 3

# A combat segment that kills the bot this many times without finishing
# is a fight the bot cannot win here (over-level content, a multi-mob
# camp, a named it can't out-DPS). Re-approaching it is what turned one
# bad quest into 150 deaths overnight -- abandon the segment instead.
# 3, down from 6: abandonment is cheap (relevel gate retries the quest
# 1-2 levels stronger, it is never skipped), while each death costs
# ~2.5min of recovery -- live, dense social camps (q358 Garren's Haunt
# gnolls chain-adding on every pull) burned the full budget every pass
# with no realistic win at-level.
DEFAULT_DEATH_BUDGET = 3

# XP required to advance FROM level N to N+1, WotLK 3.3.5a (standard client
# values). Used only to turn the raw `xp=<n>` the status command reports
# (XP into the current level) into a human XP% for decision logs -- these
# bots live entirely in the 1-12 band, so the table covers it with headroom.
XP_TO_NEXT = {
    1: 400, 2: 900, 3: 1400, 4: 2100, 5: 2800, 6: 3600, 7: 4500, 8: 5400,
    9: 6500, 10: 7600, 11: 8700, 12: 10100, 13: 11400, 14: 12700, 15: 13900,
}

# A quest that FAILS its real attempts this many times (not deaths -- stalls /
# never-rewarded: unsupported use-item/interact behavior, an unreachable giver,
# a phased/event quest) is undoable at any level. Permanently skip it so the bot
# stops burning ~2min/pass re-failing it and spends its time on doable quests +
# the grind-to-target fallback. Deliberate blacklisting, distinct from the
# level-based defer a too-hard-but-winnable fight gets.
DEFER_FAIL_LIMIT = 1
# A chain-PREREQUISITE quest gets more chances than a leaf quest before being
# skipped -- skipping it cascade-drops its whole chain, so a transient/coord
# failure must not. But it is NOT infinite: a genuinely undoable prereq (e.g.
# q376 with spread-thin scavengers) that never completes would otherwise burn
# its full stall budget every pass forever, starving the grind tiers and
# stranding the bot. After this many full-segment failures, accept the chain
# loss and skip it so the bot proceeds to its grind ladder + other content.
PREREQ_FAIL_LIMIT = 3


class SegmentAbandoned(Exception):
    """Raised by a combat segment when its per-segment death budget is
    spent -- the driver skips the segment rather than feeding the loop."""


class GrindYield(Exception):
    """Raised by grind_to_level when the level it just reached unlocks a
    quest segment deferred earlier this pass -- quest XP is faster and
    safer than continuing an even-level grind (live: Humantwelve kept
    wolf-grinding at 6, dying ~1/cycle, while its whole unlocked level-6
    Northshire quest block sat deferred). The driver re-defers the grind
    (no relevel gate, not done) so it resumes after the quests run."""


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Runner:
    def __init__(self, cfg: Config, route: dict, state_path: str):
        self.cfg = cfg
        self.route = route
        self.char = route["char"]
        self.account = route["account"]
        # Route family (e.g. "durotar_orc_warrior_1_12"), set by main() from
        # the route filename; used in structured decision logs so the
        # metrics report can group by family without re-deriving it.
        self.family = route.get("family", "?")
        self.state_path = state_path
        self.state = {"done": [], "level_history": [], "deaths": 0,
                      "segment_attempts": {}}
        # True while recover_from_death is driving the ghost-walk --
        # wait_guide's own death check must not treat the deliberate
        # ghost state as a fresh death and recurse into recovery.
        self.recovering = False
        # The segment currently executing (set by run()) -- recovery
        # uses its unstick hub to break corpse-camp death loops.
        self.current_seg = None
        # Threat-aware transit planner (optional: requires safe_path.py +
        # threat_spawns.json next to this script). None -> direct walks.
        try:
            import safe_path
            self._safe_path = safe_path
        except Exception:
            self._safe_path = None
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
        # Named form returns only this bot's block on servers with the
        # status filter; older servers dump everyone -- the block
        # slicing below is correct for both.
        out = self.ap(f"status {self.char}")
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
        # The status dump lists EVERY registered bot, each followed by
        # its own optional corpse line -- so the corpse search must be
        # confined to this bot's block. Searching the whole dump made
        # every ghost chase the FIRST dead bot's corpse (live case:
        # the Elwynn mage ghost-marching toward the orc warrior's
        # Durotar corpse; fleet-wide cross-zone scatter followed).
        block = out[m.end():]
        nb = re.search(r"\n\s+\w+ lvl \d+ map \d+ pos", block)
        if nb:
            block = block[:nb.start()]
        c = re.search(r"corpse at \(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", block)
        if c:
            status["corpse"] = (float(c.group(1)), float(c.group(2)), float(c.group(3)))
        return status

    def quest_state(self, quest_id: int) -> dict:
        out = self.ap(f"queststatus {self.char} {quest_id}")
        m = re.search(r"status for '[^']+': (\d+) \(rewarded=(\w+)\) lvl=(\d+) xp=(\d+)", out)
        if not m:
            raise RuntimeError(f"unparseable queststatus: {out.strip()[:200]}")
        cc = re.search(r"canComplete=(\w+)", out)
        return {"status": int(m.group(1)), "rewarded": m.group(2) == "true",
                "level": int(m.group(3)), "xp": int(m.group(4)),
                "can_complete": bool(cc) and cc.group(1) == "true"}

    def level(self) -> int:
        return self.quest_state(788)["level"]  # any quest id works; lvl always printed

    def xp_pct(self) -> int:
        """Percent of the way through the current level (0-99), or -1 if
        unknown. Cheap best-effort for decision logs -- one status read."""
        qs = self.quest_state(788)
        need = XP_TO_NEXT.get(qs["level"])
        if not need:
            return -1
        return max(0, min(99, int(100 * qs["xp"] / need)))

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
        # If the bot is ALREADY a ghost before we release (a prior
        # recovery wandered it -- live case: 3700yd into the
        # mountains), its position is NOT a graveyard: route via the
        # segment hub first so the corpse approach starts from the
        # known-pathable network.
        pre = self.bot_status()
        was_already_ghost = pre.get("ghost", False)
        # Every death is a real hazard signal at THIS location, whatever
        # segment happened to be nominally active -- record it into the
        # same hard_spots avoidance list the segment-budget condemnation
        # path already uses (see the SegmentAbandoned handling below and
        # grind_camp_for_level's condemned()). Overnight 2026-07-08 live
        # evidence for why this needed to exist as its own hook: several
        # bots (Fizzlewick 9 deaths, Nelfhunter 15, Nelfdruid 12) were
        # repeatedly ambushed by the SAME dense wildlife camp (a
        # Frostmane bear/leopard den, a Barrens troll camp, a Teldrassil
        # sprite pair) via ambient self-defense while cycling through
        # several DIFFERENT nearby quest/grind segments -- each
        # individual segment's own death budget (3 for quests, 12 for
        # grinds) never saw enough same-segment deaths to trip, because
        # the deaths were real but scattered across segments, not
        # concentrated in one. The location was dangerous; no single
        # segment's counter ever found out. Recording here closes that
        # gap at its source: any segment that later targets within 250yd
        # of a real death, at or below the level it happened at, defers
        # (quest path) or falls back to a lower-tier camp (grind path)
        # exactly like an explicit budget-exceeded condemnation would.
        if pre.get("online") and pre.get("x") is not None:
            self.state.setdefault("hard_spots", []).append(
                [pre["x"], pre["y"], self.level()])
            self.save_state()
        released_at = time.time()
        self.ap(f"releasespirit {self.char}")
        time.sleep(3.0)
        st = self.bot_status()
        # A fresh release spawns the ghost AT a graveyard -- remember
        # it: the only place a spirit healer exists, and the anchor to
        # return to if the corpse walk diverges.
        graveyard = None if was_already_ghost else \
            ((st.get("x"), st.get("y"), st.get("z")) if st.get("online") else None)
        if was_already_ghost and self.current_seg is not None:
            point = self.current_seg.get("unstick") or self.route.get("unstick")
            hub = HUBS.get(point)
            if hub:
                log("stale ghost detected -- routing via segment hub first")
                self.walk_toward(hub[0], hub[1], hub[2], arrive_within=25.0,
                                 max_issues=40, allow_ghost=True)
        corpse = st.get("corpse")
        if corpse:
            # Ghost-walk back with the same bisecting walker every
            # other movement uses -- the graveyard can be several
            # hundred yards out, far beyond a single MoveTo's silent
            # path-length limit. CRUCIAL (found via the overnight
            # death-spiral data): do NOT reclaim ON the death spot.
            # Ghosts are unattackable -- position at the corpse's
            # EDGE, ~30yd toward the segment's safe hub but still
            # inside the 39yd reclaim radius, exactly like a real
            # player edging their resurrect away from the camp. The
            # old flow resurrected at 50% health in the middle of the
            # camp and then WALKED through it to the hub, which fed a
            # fleet-wide death loop once unsticks stopped teleporting.
            gx, gy, gz = corpse
            hub = None
            if self.current_seg is not None:
                point = self.current_seg.get("unstick") or self.route.get("unstick")
                hub = HUBS.get(point)
            if hub:
                dx, dy = hub[0] - corpse[0], hub[1] - corpse[1]
                dist = (dx * dx + dy * dy) ** 0.5 or 1.0
                gx = corpse[0] + 30.0 * dx / dist
                gy = corpse[1] + 30.0 * dy / dist
            if not self.walk_toward(gx, gy, gz,
                                    arrive_within=8.0, max_issues=30,
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
                # First death on a segment: resurrect at the corpse EDGE
                # (already pre-positioned toward the hub) and regen in
                # place -- do NOT walk through the camp at half health
                # (the teleport-era habit that became a death loop once
                # unsticks walk). But once this segment has killed the
                # bot repeatedly, the field edge is clearly still in
                # range of whatever keeps winning: fully retreat to the
                # safe hub and rest to near-full before re-approaching,
                # so each retry starts from strength instead of feeding
                # the same losing fight at 75%. The death budget caps
                # how many retries happen at all.
                # 0.9 even on a first death: resuming at 75% fed the next
                # death (live: Humantwelve died 88s after recovery, at the
                # camp, with adds -- every resume must start near-full).
                deadly = self.deaths_this_segment() >= 2
                hub = None
                if self.current_seg is not None:
                    point = self.current_seg.get("unstick") or self.route.get("unstick")
                    hub = HUBS.get(point)
                # ALWAYS clear the corpse zone before resting: the 30yd
                # reclaim-edge offset sits inside camp wander radii, and
                # resting there at 50% fed a reclaim->re-aggro->death loop
                # ~90s after every recovery (live: Tanktwelve vs Wiry
                # Swoops, Roguetwelve vs Rot Hide Graverobbers, both
                # double-dying within 2-3 min). 90yd toward the hub clears
                # wander range without walking the whole way back.
                if hub and corpse:
                    hd = ((hub[0] - corpse[0]) ** 2 + (hub[1] - corpse[1]) ** 2) ** 0.5 or 1.0
                    ex = corpse[0] + min(90.0, hd) * (hub[0] - corpse[0]) / hd
                    ey = corpse[1] + min(90.0, hd) * (hub[1] - corpse[1]) / hd
                    self.walk_toward(ex, ey, st.get("z", corpse[2]),
                                     arrive_within=12.0, max_issues=4)
                if not deadly:
                    hub = None
                if hub:
                    log(f"segment has killed the bot {self.deaths_this_segment()}x "
                        "-- retreating to hub to rest before re-approaching")
                    self.walk_toward(hub[0], hub[1], hub[2], arrive_within=20.0)
                target_frac = 0.95 if deadly else 0.9
                regen_deadline = time.time() + 180.0
                while time.time() < regen_deadline:
                    st = self.bot_status()
                    if st.get("hp", 0) >= target_frac * st.get("max_hp", 1):
                        break
                    time.sleep(10.0)
                return
            time.sleep(10.0)
        # Not fatal on its own: the caller's next death check re-enters
        # recovery (fresh corpse read, fresh ghost-walk). After two
        # full failed cycles the corpse is genuinely unreachable (live
        # case: it sank to a lake bottom at z -51) -- take the game's
        # own fallback, the spirit healer at the graveyard the ghost
        # is standing in (sickness + durability cost apply for real;
        # repair-on-sell absorbs the durability).
        # A worldserver restart mid-recovery makes every probe fail
        # without meaning anything about the corpse -- don't count
        # cycles while the bot isn't even resolvable online.
        if not self.bot_status().get("online"):
            log("recovery cycle voided: server/bot offline (deploy window)")
            return
        self.recovery_failures = getattr(self, "recovery_failures", 0) + 1
        log(f"death recovery attempt failed (cycle {self.recovery_failures}/5)")
        if self.recovery_failures >= 2:
            # The spirit healer only exists at the graveyard -- walk
            # the ghost back there first (the corpse walk may have
            # wandered it far off).
            if graveyard and graveyard[0] is not None:
                self.walk_toward(graveyard[0], graveyard[1], graveyard[2],
                                 arrive_within=20.0, allow_ghost=True)
            out = self.ap(f"spirithealres {self.char}")
            time.sleep(3.0)
            st = self.bot_status()
            if "submitted=false" in out and not st.get("alive"):
                # No healer within search radius: the ghost is stranded
                # past walking range of everything (live case: 106
                # deaths mid-Barrens, corpse unreachable, ghost walk
                # stalled). RepopAtGraveyard is the same call spirit
                # release runs -- port to the zone graveyard, where the
                # healer is by construction, and try again.
                log("no spirit healer in range -- repopping ghost to zone graveyard")
                self.ap(f"repopgraveyard {self.char}")
                time.sleep(3.0)
                out = self.ap(f"spirithealres {self.char}")
                time.sleep(3.0)
                st = self.bot_status()
            if st.get("alive") and not st.get("ghost"):
                log("recovered via SPIRIT HEALER (sickness + durability paid)")
                self.recovery_failures = 0
                # Resurrection sickness (-75% stats) applies from level
                # 10 and lasts a minute at these levels -- fighting
                # through it is suicide; sit it out at the graveyard
                # (safe ground by construction).
                if st.get("level", 1) >= 10:
                    log("waiting out resurrection sickness (70s)")
                    time.sleep(70.0)
                return
            log(f"spirit healer resurrect did not land: {out.strip()[:90]}")
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
        # Continent check: fleet bots have wandered onto TRANSPORTS
        # (the Brill zeppelin) and woken up on the wrong map, where
        # their whole route is meaningless -- droughts, wrong-zone
        # deaths, wandering ghosts. The player-legitimate way home is
        # the hearthstone (10s cast, 60min cooldown; binds to the
        # racial starting inn for never-rebound characters).
        want = self.route.get("map")
        displaced = want is not None and st.get("map") != want
        reason = f"on map {st.get('map')}, route wants {want}"
        if not displaced:
            # Same continent but grossly off-route also counts: fleet
            # bots ghost-marched 2000yd+ into the Barrens, where a
            # level-6 walk home is a death loop. No route segment puts
            # a bot 1500yd from its current anchor legitimately.
            anchor = self.segment_anchor(getattr(self, "current_seg", None))
            if anchor:
                d = math.hypot(st["x"] - anchor[0], st["y"] - anchor[1])
                if d > 1500.0:
                    displaced = True
                    reason = f"{d:.0f}yd from segment anchor {anchor}"
        if displaced:
            log(f"DISPLACED ({reason}) -- hearthing")
            # The 10s hearth cast dies to movement: a still-running
            # MoveTo guide re-issues every tick (live case: cast
            # "landed" by map but the bot never left the Barrens).
            # Starting a MoveTo at the bot's own feet replaces any
            # running guide and finishes instantly -- a stop button.
            self.ap(f"guidestartmoveto {self.char} {st['x']:.1f} {st['y']:.1f} {st['z']:.1f}")
            time.sleep(2.0)
            self.ap(f"hearth {self.char}")
            time.sleep(14.0)
            st2 = self.bot_status()
            landed = st2.get("online") and (want is None or st2.get("map") == want)
            if landed and st2.get("map") == st.get("map"):
                # Same map before and after: only real movement proves
                # the cast went off (the bind inn is near route start).
                moved = math.hypot(st2["x"] - st["x"], st2["y"] - st["y"])
                landed = moved > 100.0
            if landed:
                log(f"hearthstone landed: map {st2.get('map')} pos ({st2.get('x')}, {st2.get('y')})")
            else:
                log(f"hearth did not land (map {st2.get('map')} pos ({st2.get('x')}, {st2.get('y')}));"
                    " interrupted or on cooldown -- will retry next check")
                time.sleep(30.0)
            return True
        return False

    @staticmethod
    def segment_anchor(seg: dict | None) -> tuple[float, float] | None:
        """Best-effort (x, y) a bot working this segment should be near."""
        if not seg:
            return None
        if seg.get("type") == "walk" and seg.get("hops"):
            hx, hy = seg["hops"][-1][0], seg["hops"][-1][1]
            return (hx, hy)
        for xk, yk in (("x", "y"), ("giver_x", "giver_y"), ("turnin_x", "turnin_y")):
            x, y = seg.get(xk), seg.get(yk)
            if x is not None and y is not None and (x or y):
                return (float(x), float(y))
        entries = seg.get("kill_entries")
        if entries:
            return (float(entries[0]["x"]), float(entries[0]["y"]))
        return None

    def ensure_bag_space(self, seg: dict | None) -> bool:
        """Global bag-pressure rule (applies to every segment/state): keep a
        working margin of free bag slots. When free is low, stop what we're
        doing and go vendor with the expanded junk policy before adding any
        more items -- a full bag silently drops quest loot and wedges turn-ins.
        Threshold raised from 2 to 6: starter 16-slot bags on low-level bots
        fill fast with quest drops + whites, and selling only at <=2 free was
        too late (the loot that filled the last slots was already lost).
        Returns True when there's room to proceed, False if still clogged
        after vendoring (emergency: stay in vendor/recovery)."""
        g = self.guide_status()
        free = g.get("free_bag_slots", 99)
        if free > 6:
            return True
        vendor = (seg.get("vendor") if seg else None) or self.route.get("home_vendor")
        step = seg.get("id", "?") if seg else "?"
        vid = vendor.get("vendor") if vendor else None
        log(f"BAG PRESSURE: {self.char} free={free} (<=2) step={step} "
            f"-> nearest vendor {vid}")
        if not vendor:
            log("bag pressure: no vendor for this segment -- cannot vendor")
            return False
        self.seg_sell(vendor)
        after = self.guide_status().get("free_bag_slots", free)
        log(f"bag pressure: vendored at {vid}, free {free} -> {after}")
        if after > 2:
            return True
        # still under the hard floor after a vendor pass -> emergency below
        # Emergency: still clogged after selling -- surface exactly what
        # is filling the bags and remain in vendor/recovery rather than
        # continuing to quest into a wall.
        info = " ".join(self.ap(f"baginfo {self.char}").split())
        log(f"BAG PRESSURE UNRESOLVED (free={after} after vendor); "
            f"top items: {info[:400]}")
        return False

    def equip_upgrades(self) -> None:
        """Review looted gear: equip anything better (or fill an empty
        slot); the replaced piece drops to bags and is vendored on the
        next run (expanded junk policy). MUST run continuously during
        grinds, not just at segment end -- a long grind_to_level/
        quest_grind otherwise never gears up (live: Grunt stuck ilvl 5,
        9 empty slots, at level 10 for hours because cr-grind-to-12
        never triggered an equip pass)."""
        out = self.ap(f"equipupgrades {self.char}")
        m = re.search(r"Equipped (\d+) upgrade", out)
        if m and m.group(1) != "0":
            log(f"equipped {m.group(1)} upgrade(s) from loot")

    def deaths_this_segment(self) -> int:
        return self.state["deaths"] - getattr(self, "seg_death_baseline", 0)

    def check_death_budget(self, seg: dict, default: int = DEFAULT_DEATH_BUDGET) -> None:
        budget = seg.get("death_budget", default)
        n = self.deaths_this_segment()
        if n >= budget:
            raise SegmentAbandoned(f"died {n} times on this segment (budget {budget})")

    def wait_for_health(self, fraction: float = 0.85, timeout: float = 150.0) -> None:
        """Never start a fight half-dead -- reclaims and chained adds
        otherwise walk straight into the next death (observed live:
        recovery -> jumped mid-regen at 17/146 -> dead again)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            st = self.bot_status()
            if not st.get("online") or not st.get("alive"):
                return
            if st.get("hp", 0) >= fraction * st.get("max_hp", 1):
                return
            time.sleep(10.0)

    def at_wrong_layer(self, x: float, y: float, z: float) -> bool:
        """KNOWN_FAILURES.md #30's signature, generalized: 2D-at an
        authored NPC coordinate but several yards ABOVE it (tent roofs,
        burrow tops, tower floors). Only meaningful for REAL NPC
        coordinates (authored z is exact); never use for guessed walk
        hops."""
        st = self.bot_status()
        if not st.get("online"):
            return False
        d2d = ((st.get("x", 1e9) - x) ** 2 + (st.get("y", 1e9) - y) ** 2) ** 0.5
        return d2d < 15.0 and st.get("z", 0.0) - z > 4.0

    def unstick(self, seg: dict, key: str = "unstick") -> bool:
        """The /stuck equivalent: teleport to the segment's authored
        hub tele-point after walking has genuinely given up (real
        stranding observed live: chasing quest mobs up a ridge put the
        bot on a mesa whose polys don't path back down). Used at most
        once per 120s so a genuinely broken segment still fails loudly
        instead of teleport-looping. Counted and reported honestly."""
        point = seg.get(key) or seg.get("unstick") or self.route.get("unstick")
        if not point:
            return False
        now = time.time()
        if now - getattr(self, "_last_unstick", 0.0) < 120.0:
            return False
        self._last_unstick = now
        self.state["unsticks"] = self.state.get("unsticks", 0) + 1
        self.save_state()
        # THE RULE: bots do not teleport. An unstick is a WALK to the
        # hub (real navigation; the module now bisects long legs and
        # compass-escapes pinned spots itself). Teleporting remains
        # available ONLY behind --allow-tele for emergencies, so any
        # walking failure surfaces as a loud, fixable stranding
        # instead of being silently papered over.
        hub = HUBS.get(point)
        if hub and self.walk_toward(hub[0], hub[1], hub[2], arrive_within=25.0,
                                    allow_ghost=True):
            log(f"UNSTICK #{self.state['unsticks']}: walked to {point}")
            return True
        if ALLOW_TELE:
            log(f"UNSTICK #{self.state['unsticks']}: WALK FAILED -- emergency tele to {point}")
            self.soap(f".tele name {self.char} {point}")
            time.sleep(3.0)
            self.ap(f"logout {self.char}")
            time.sleep(4.0)
            self.ensure_online()
            return True
        log(f"UNSTICK #{self.state['unsticks']}: STRANDED (walk to {point} failed, "
            f"teleport disabled) -- surfacing as a failure")
        return False

    # ------------------------------------------------------ guide waits

    def live_threats(self) -> list:
        """Live hostile creatures near the bot (engine faction truth), as
        [[x, y, level], ...] planner threat points -- the HB
        AvoidanceManager idea: static spawn anchors miss wandering
        patrols, live positions don't. Empty on older servers without
        the `threats` command (planner then uses the static field only)."""
        try:
            out = self.ap(f"threats {self.char} 250")
        except Exception:
            return []
        pts = []
        for m in re.finditer(
                r"threat entry=\d+ level=(\d+) pos=\(([-\d.]+), ([-\d.]+), [-\d.]+\)", out):
            pts.append([float(m.group(2)), float(m.group(3)), int(m.group(1))])
        return pts

    def walk_toward(self, x: float, y: float, z: float,
                    arrive_within: float = 25.0, max_issues: int = 10,
                    allow_ghost: bool = False) -> bool:
        """Walk to a point, routing long living-bot legs around mob camps.

        Long walks on the raw navmesh cut straight through camps -- the
        dominant fleet death class once in-camp behavior was fixed (live:
        Humantwelve's corpses all along the Goldshire->wolf-camp line;
        Grunttwelve dying mid-vendor-run in the Razormane belt). For legs
        over 150yd, safe_path plans hops through the coldest corridor of
        the exported threat field (roads are naturally spawn-free lanes);
        each hop then walks with the normal machinery below. Any planning
        or hop failure falls back to the direct leg, so this can only
        remove risk. Ghost walks skip it -- ghosts are unattackable and
        the corpse bearing matters more than safety.
        """
        if not allow_ghost and not getattr(self, "_on_planned_path", False):
            st = self.bot_status()
            if st.get("online") and st.get("alive") and not st.get("ghost"):
                dist = ((st["x"] - x) ** 2 + (st["y"] - y) ** 2) ** 0.5
                if dist > 150.0 and self._safe_path is not None:
                    hops = None
                    try:
                        # A location that has ALREADY killed this bot is a
                        # stronger signal than a live-perceived patrol --
                        # feed hard_spots into the same extra_threats
                        # channel so the corridor planner routes around
                        # proven danger, not just what's visible right now.
                        # Real gap this closes (live 2026-07-08): hard_spots
                        # already fed camp/quest-target SELECTION (see
                        # condemned() and the quest_delivery deferral
                        # check), but the corridor planner used for the
                        # WALK between two unrelated, individually-fine
                        # objectives never consulted it -- Nelfhunter and
                        # Grunttwelve each died repeatedly mid-transit
                        # through a spot hard_spots had already recorded
                        # 6-17 deaths at, because nothing routing the walk
                        # itself knew to avoid it.
                        extra = self.live_threats() + \
                            [list(hs[:3]) for hs in getattr(self, "hard_spots", [])]
                        hops = self._safe_path.plan(
                            st["map"], st["x"], st["y"], x, y, st["level"],
                            extra_threats=extra,
                            blackspots=self.route.get("blackspots"))
                    except Exception as exc:  # planner must never kill a walk
                        log(f"safe_path: planning failed ({exc}); walking direct")
                    if hops:
                        log(f"safe_path: {len(hops)} hop(s) around threat to "
                            f"({x:.0f},{y:.0f})")
                        self._on_planned_path = True
                        try:
                            # A failed hop skips to the NEXT hop rather than
                            # abandoning the corridor: falling back to the
                            # direct leg forfeits the whole detour (live:
                            # Korgath's fallback beelined through the
                            # Razormane fields and died at level 2). Only
                            # after 3 consecutive unreachable hops is the
                            # plan genuinely wrong for the navmesh here.
                            failed = 0
                            for i, (hx, hy, hz) in enumerate(hops):
                                if not hz:
                                    frac = (i + 1) / (len(hops) + 1)
                                    hz = st["z"] + (z - st["z"]) * frac
                                if self.walk_toward(hx, hy, hz,
                                                    arrive_within=25.0,
                                                    max_issues=4):
                                    failed = 0
                                    continue
                                failed += 1
                                log(f"safe_path: hop {i + 1}/{len(hops)} "
                                    f"unreachable ({failed} consecutive)")
                                if failed >= 3:
                                    log("safe_path: corridor abandoned; "
                                        "walking direct")
                                    break
                        finally:
                            self._on_planned_path = False
        return self._walk_leg(x, y, z, arrive_within, max_issues, allow_ghost)

    def _walk_leg(self, x: float, y: float, z: float,
                  arrive_within: float = 25.0, max_issues: int = 10,
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
        best_dist = None
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
            # Divergence leash: compass escapes count as "progress"
            # even when they wander AWAY, and an escape-assisted walk
            # can migrate thousands of yards (live: a ghost drifted
            # 3700yd into the mountains chasing an unpathable corpse
            # bearing). If we're ever 60yd worse than our best
            # approach, the target is not walkable from here -- stop.
            if best_dist is None or dist < best_dist:
                best_dist = dist
            elif dist > best_dist + 60.0:
                log(f"walk_toward ({x:.0f},{y:.0f}): diverging "
                    f"({dist:.0f}yd vs best {best_dist:.0f}yd) -- aborting")
                return False
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
        #
        # Scale the budget with the number of kill points: a collection quest
        # changes no XP/status until FULLY complete, so every roam attempt reads
        # as a stall even while quietly gathering items -- a flat budget of 2
        # fails a spread-thin roam circuit (q376's 4 scavenger points) before it
        # visits enough points to collect all N. One attempt per point + 2; a
        # single-point (dense / undoable) quest keeps the fast budget.
        stall_budget = seg.get("attempts", max(2, len(kill_entries) + 2))
        stalls = 0
        attempt = 0
        last_xp = self.quest_state(q)["xp"]
        while stalls < stall_budget:
            self.check_death_budget(seg)
            attempt += 1
            qs = self.quest_state(q)
            if qs["rewarded"]:
                return True
            # Objectives met but the status never flipped to COMPLETE
            # (engine-credited kills on multi-objective quests stick at
            # INCOMPLETE for these bots). Flip it so the turn-in branch
            # below fires instead of grinding already-maxed objectives
            # forever (live: Grunt's 784 stuck at 10/8 killed).
            if qs["status"] != QUEST_STATUS_COMPLETE and qs.get("can_complete"):
                log(f"quest {q} objectives met but status {qs['status']} -- flipping to COMPLETE")
                self.ap(f"completequest {self.char} {q}")
                qs = self.quest_state(q)
            if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                # Not accepted yet -- the guide's AcceptQuest step only
                # searches 100yd, so get near the giver first. An NPC
                # under an overhang (e.g. the Den burrow) must be
                # approached VIA a ground-level point in front of it,
                # and the runner must STOP at the via: the guide's own
                # AcceptQuest approach then walks the whole final leg
                # from the proven side, exactly like the turn-in
                # geometry (walking to within 60yd ourselves put the
                # Troll's accept on Gornek's roof ten attempts in a
                # row, while every via-anchored turn-in worked).
                via = seg.get("giver_via")
                if via:
                    self.walk_toward(via[0], via[1], via[2], arrive_within=3.0)
                else:
                    self.walk_toward(seg["giver_x"], seg["giver_y"], seg["giver_z"],
                                     arrive_within=60.0)
            if qs["status"] == QUEST_STATUS_COMPLETE:
                # Objectives done -- put the waypoint at the turn-in NPC
                # and pre-walk there, so the walk-back happens even when
                # the grind field is beyond TurnInQuest's 150yd search
                # radius (KNOWN_FAILURES.md #28).
                via = seg.get("turnin_via")
                if via:
                    if not self.walk_toward(via[0], via[1], via[2], arrive_within=3.0):
                        self.unstick(seg, key="turnin_unstick")
                        self.walk_toward(via[0], via[1], via[2], arrive_within=3.0)
                    wp = (via[0], via[1], via[2])
                else:
                    wp = (seg["turnin_x"], seg["turnin_y"], seg["turnin_z"])
                    if not self.walk_toward(wp[0], wp[1], wp[2], arrive_within=60.0):
                        self.unstick(seg, key="turnin_unstick")
                        self.walk_toward(wp[0], wp[1], wp[2], arrive_within=60.0)
                ke = kill_entries[0]
            else:
                ke = kill_entries[(attempt - 1) % len(kill_entries)]
                wp = (ke["x"], ke["y"], ke["z"])
                if qs["status"] == QUEST_STATUS_INCOMPLETE:
                    # Already accepted: pre-walk to the kill field so
                    # the guide's bounded MoveTo arrives instantly.
                    # Leaving a via-NPC's pocket needs the same
                    # ground-level detour as approaching it (#30 works
                    # both ways) -- but ONLY when actually near the
                    # giver; taking the detour from across the zone
                    # walks away from the field for nothing.
                    via = seg.get("giver_via")
                    st = self.bot_status()
                    near_giver = (via and st.get("online") and
                                  ((st["x"] - seg["giver_x"]) ** 2 +
                                   (st["y"] - seg["giver_y"]) ** 2) ** 0.5 < 100.0)
                    if near_giver:
                        self.walk_toward(via[0], via[1], via[2], arrive_within=3.0)
                    if not self.walk_toward(wp[0], wp[1], wp[2], arrive_within=15.0):
                        self.unstick(seg)
                # NOT accepted: stay at the giver -- the guide's
                # AcceptQuest step only searches 100yd, so walking to
                # the field first makes the accept impossible (found
                # live: quest 792 never got accepted because this
                # pre-walk dragged the bot 490yd away before the guide
                # ran). The guide accepts here; its own MoveTo starts
                # the walk and the NEXT attempt (status now
                # INCOMPLETE) pre-walks the rest.
            self.wait_for_health()
            spell = seg.get("spell", self.route.get("opportunistic_spell", 0))
            heal = seg.get("heal_spell", self.route.get("heal_spell", 0))
            result = self.issue_and_wait(
                f"guidestartquestgrind {self.char} {q} {seg['giver']} {ke['entry']} "
                f"{seg['turnin']} {seg.get('choice', 0)} {wp[0]:.1f} {wp[1]:.1f} {wp[2]:.1f} {spell} {heal}",
                seg.get("wall_timeout", 400))
            qs_after = self.quest_state(q)
            progressed = (qs_after["xp"] != last_xp or qs_after["level"] > qs["level"]
                          or qs_after["status"] != qs["status"] or qs_after["rewarded"])
            last_xp = qs_after["xp"]
            stalls = 0 if progressed else stalls + 1
            # Vertical-layer trap detector (KNOWN_FAILURES.md #30): a
            # failed attempt with the bot 2D-at its destination (the
            # turn-in NPC, or the kill-field waypoint of a cave
            # interior) but several yards ABOVE it means we're standing
            # on the terrain layer over it -- no amount of re-issuing
            # fixes that. Unstick to the segment's authored point
            # (which for cave content is an INTERIOR coordinate).
            if not progressed:
                st = self.bot_status()
                d2d = ((st.get("x", 1e9) - wp[0]) ** 2 +
                       (st.get("y", 1e9) - wp[1]) ** 2) ** 0.5
                if d2d < 25.0 and st.get("z", 0.0) - wp[2] > 4.0:
                    log(f"quest {q}: layer trap at destination (z +"
                        f"{st['z'] - wp[2]:.1f}) -- unsticking")
                    self.unstick(seg)
                elif self.at_wrong_layer(seg["giver_x"], seg["giver_y"], seg["giver_z"]):
                    log(f"quest {q}: layer trap at the GIVER -- unsticking")
                    self.unstick(seg)
            log(f"quest {q} attempt {attempt} (entry {ke['entry']}): {result}"
                f" (stalls {stalls}/{stall_budget})")
            if qs_after["rewarded"]:
                return True
            g = self.guide_status()
            if g.get("turn_in_refused"):
                log(f"quest {q}: turn-in refused -- selling junk first")
                self.seg_sell(seg.get("vendor", self.route["home_vendor"]))
            # Review loot each cycle: equip upgrades, then vendor the
            # replaced/junk gear before the next kill (bag pressure).
            self.equip_upgrades()
            self.ensure_bag_space(seg)
        return self.quest_state(q)["rewarded"]

    def seg_quest_accept(self, seg: dict) -> bool:
        q = seg["quest"]
        qs = self.quest_state(q)
        if qs["rewarded"] or qs["status"] in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
            return True
        for attempt in range(seg.get("attempts", 2)):
            via = seg.get("via")
            if via and not self.walk_toward(via[0], via[1], via[2], arrive_within=3.0):
                self.unstick(seg)
            if not self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=3.0):
                self.unstick(seg)
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
        qs = self.quest_state(q)
        if qs["rewarded"]:
            return True
        if qs["status"] == QUEST_STATUS_NONE:
            # Live case (Locktwelve, quest 823): a verified accept
            # later reads status NONE -- the quest left the log with
            # no breadcrumb/timer/capacity explanation (KNOWN_FAILURES
            # open item). Whatever the cause, the recovery is what a
            # player would do: go back to the giver and take it again.
            for other in self.route["segments"]:
                if other.get("type") == "quest_accept" and other.get("quest") == q:
                    log(f"quest {q} vanished from log after a verified accept"
                        " -- re-accepting from paired giver")
                    if not self.seg_quest_accept(other):
                        log(f"re-accept of quest {q} failed")
                    break
        for attempt in range(seg.get("attempts", 2)):
            via = seg.get("via")
            if via and not self.walk_toward(via[0], via[1], via[2], arrive_within=3.0):
                self.unstick(seg)
            if not self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=3.0):
                self.unstick(seg)
                self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=3.0)
            self.ap(f"turnin {self.char} {q} {seg['turnin']} {seg.get('choice', 0)}")
            time.sleep(2.0)
            if self.quest_state(q)["rewarded"]:
                return True
            log(f"turnin quest {q} attempt {attempt + 1}: not rewarded yet")
            if self.check_alive_or_recover():
                continue
        return self.quest_state(q)["rewarded"]

    def seg_quest_delivery(self, seg: dict) -> bool:
        """Accept a no-objective quest at its giver and hand it in at its ender
        as one atomic, deferrable unit (report / talk / auto-given-item quests).
        Split accept+turnin segments were fragile: a min_level-deferred accept
        still let the paired turnin run and fail. Here accept and turn-in share
        one segment and one min_level gate. A quest that auto-completes+rewards
        on accept, or that cannot complete (item must be gathered elsewhere), is
        handled without churning -- the latter simply defers for a later pass."""
        q = seg["quest"]
        if self.quest_state(q)["rewarded"]:
            return True
        for attempt in range(seg.get("attempts", 2)):
            self.check_death_budget(seg)
            qs = self.quest_state(q)
            if qs["rewarded"]:
                return True
            if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                via = seg.get("giver_via")
                if via and not self.walk_toward(via[0], via[1], via[2], arrive_within=3.0):
                    self.unstick(seg)
                if not self.walk_toward(
                        seg["giver_x"], seg["giver_y"], seg["giver_z"], arrive_within=5.0):
                    self.unstick(seg)
                self.ap(f"acceptquest {self.char} {q} {seg['giver']}")
                time.sleep(2.0)
                qs = self.quest_state(q)
                if qs["rewarded"]:  # auto-completed and rewarded on accept
                    return True
                if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                    log(f"delivery quest {q}: accept failed; not walking to turn-in")
                    continue
            if qs.get("can_complete") and qs["status"] != QUEST_STATUS_COMPLETE:
                self.ap(f"completequest {self.char} {q}")
            via = seg.get("turnin_via")
            if via and not self.walk_toward(via[0], via[1], via[2], arrive_within=3.0):
                self.unstick(seg, key="turnin_unstick")
            if not self.walk_toward(
                    seg["turnin_x"], seg["turnin_y"], seg["turnin_z"], arrive_within=5.0):
                self.unstick(seg, key="turnin_unstick")
            self.ap(f"turnin {self.char} {q} {seg['turnin']} {seg.get('choice', 0)}")
            time.sleep(2.0)
            if self.quest_state(q)["rewarded"]:
                return True
            log(f"delivery quest {q} attempt {attempt + 1}: not rewarded yet")
            if self.check_alive_or_recover():
                continue
        return self.quest_state(q)["rewarded"]

    def _go_progress(self) -> int:
        """Live objective progress the InteractGameObject step reports
        (`questAction: progress=N`) -- used to tell a still-collecting run
        from a genuinely stalled one, since gameobject loot moves no XP or
        quest status until the whole objective completes."""
        out = self.ap(f"guidestatus {self.char}")
        m = re.search(r"progress=(\d+)", out)
        return int(m.group(1)) if m else 0

    def seg_quest_gameobject(self, seg: dict) -> bool:
        """Accept -> collect from a gameobject cluster (guidestartgameobject,
        multi-entry via go_entries) -> turn in. Mirror of seg_quest_grind for
        quests whose objective is using/looting world objects (chests, mineral
        veins, quest props). Live-verified path: q3902 Scavenging Deathknell."""
        q = seg["quest"]
        if self.quest_state(q)["rewarded"]:
            log(f"quest {q} already rewarded; skipping")
            return True
        go_entries = seg.get("go_entries") or [
            {"entry": seg["go_entry"], "x": seg["x"], "y": seg["y"], "z": seg["z"]}]
        radius = seg.get("radius", 120.0)
        stall_budget = seg.get("attempts", 2)
        stalls = 0
        attempt = 0
        last_xp = self.quest_state(q)["xp"]
        last_prog = 0
        while stalls < stall_budget:
            self.check_death_budget(seg)
            attempt += 1
            qs = self.quest_state(q)
            if qs["rewarded"]:
                return True
            if qs["status"] != QUEST_STATUS_COMPLETE and qs.get("can_complete"):
                self.ap(f"completequest {self.char} {q}")
                qs = self.quest_state(q)
            if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                # accept at the giver (guide AcceptQuest only searches 100yd)
                via = seg.get("giver_via")
                if via:
                    self.walk_toward(via[0], via[1], via[2], arrive_within=3.0)
                self.walk_toward(seg["giver_x"], seg["giver_y"], seg["giver_z"],
                                 arrive_within=6.0)
                self.ap(f"acceptquest {self.char} {q} {seg['giver']}")
                time.sleep(2.0)
                qs = self.quest_state(q)
                if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                    log(f"quest {q} GO: accept failed; not walking to object cluster")
                    stalls += 1
                    continue
            if qs["status"] == QUEST_STATUS_COMPLETE:
                via = seg.get("turnin_via")
                if via and not self.walk_toward(
                        via[0], via[1], via[2], arrive_within=3.0):
                    self.unstick(seg, key="turnin_unstick")
                wp = (seg["turnin_x"], seg["turnin_y"], seg["turnin_z"])
                if not self.walk_toward(wp[0], wp[1], wp[2], arrive_within=6.0):
                    self.unstick(seg, key="turnin_unstick")
                    self.walk_toward(wp[0], wp[1], wp[2], arrive_within=6.0)
                self.ap(f"turnin {self.char} {q} {seg['turnin']} {seg.get('choice', 0)}")
                time.sleep(2.0)
                if self.quest_state(q)["rewarded"]:
                    return True
                stalls += 1
                continue
            # INCOMPLETE: walk to a gameobject cluster and run the GO step,
            # which internally sweeps every reachable object of that entry.
            ge = go_entries[(attempt - 1) % len(go_entries)]
            self.wait_for_health()
            if not self.walk_toward(ge["x"], ge["y"], ge["z"], arrive_within=20.0):
                self.unstick(seg)
            result = self.issue_and_wait(
                f"guidestartgameobject {self.char} {q} {ge['entry']} {radius:.0f}",
                seg.get("wall_timeout", 150))
            qs_after = self.quest_state(q)
            prog = self._go_progress()
            progressed = (qs_after["xp"] != last_xp or prog > last_prog
                          or qs_after["status"] != qs["status"]
                          or qs_after.get("can_complete") or qs_after["rewarded"])
            last_xp = qs_after["xp"]
            last_prog = max(last_prog, prog)
            stalls = 0 if progressed else stalls + 1
            log(f"quest {q} GO attempt {attempt} (entry {ge['entry']}): {result}"
                f" progress={prog} (stalls {stalls}/{stall_budget})")
            self.equip_upgrades()
            self.ensure_bag_space(seg)
        return self.quest_state(q)["rewarded"]

    def seg_quest_useitem_unit(self, seg: dict) -> bool:
        """Accept -> use a quest item on creatures (guidestartuseitemunit) ->
        turn in. For quests whose objective is item-use, not kills (e.g. q5441
        Lazy Peons: Foreman's Blackjack 16114 on Sleeping Peons 10556 -- the
        peons never die, and the use-spell only lands on condition-qualifying
        targets, so a kill grind can never credit)."""
        q = seg["quest"]
        if self.quest_state(q)["rewarded"]:
            log(f"quest {q} already rewarded; skipping")
            return True
        clusters = seg.get("clusters") or [
            {"x": seg["x"], "y": seg["y"], "z": seg["z"]}]
        radius = seg.get("radius", 120.0)
        stall_budget = seg.get("attempts", 3)
        stalls = 0
        attempt = 0
        last_prog = 0
        while stalls < stall_budget:
            self.check_death_budget(seg)
            attempt += 1
            qs = self.quest_state(q)
            if qs["rewarded"]:
                return True
            if qs["status"] != QUEST_STATUS_COMPLETE and qs.get("can_complete"):
                self.ap(f"completequest {self.char} {q}")
                qs = self.quest_state(q)
            if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                via = seg.get("giver_via")
                if via:
                    self.walk_toward(via[0], via[1], via[2], arrive_within=3.0)
                self.walk_toward(seg["giver_x"], seg["giver_y"], seg["giver_z"],
                                 arrive_within=6.0)
                self.ap(f"acceptquest {self.char} {q} {seg['giver']}")
                time.sleep(2.0)
                qs = self.quest_state(q)
                if qs["status"] not in (QUEST_STATUS_COMPLETE, QUEST_STATUS_INCOMPLETE):
                    log(f"quest {q} useitem: accept failed; not walking to targets")
                    stalls += 1
                    continue
            if qs["status"] == QUEST_STATUS_COMPLETE:
                via = seg.get("turnin_via")
                if via and not self.walk_toward(
                        via[0], via[1], via[2], arrive_within=3.0):
                    self.unstick(seg, key="turnin_unstick")
                wp = (seg["turnin_x"], seg["turnin_y"], seg["turnin_z"])
                if not self.walk_toward(wp[0], wp[1], wp[2], arrive_within=6.0):
                    self.unstick(seg, key="turnin_unstick")
                    self.walk_toward(wp[0], wp[1], wp[2], arrive_within=6.0)
                self.ap(f"turnin {self.char} {q} {seg['turnin']} {seg.get('choice', 0)}")
                time.sleep(2.0)
                if self.quest_state(q)["rewarded"]:
                    return True
                stalls += 1
                continue
            # INCOMPLETE: walk to a target cluster (rotating like GO entries:
            # a cluster with no currently-qualifying targets, e.g. all peons
            # awake, must not wedge the segment) and run the use-item step,
            # which sweeps condition-qualifying creatures in radius.
            cl = clusters[(attempt - 1) % len(clusters)]
            self.wait_for_health()
            if not self.walk_toward(cl["x"], cl["y"], cl["z"], arrive_within=20.0):
                self.unstick(seg)
            result = self.issue_and_wait(
                f"guidestartuseitemunit {self.char} {q} {seg['item']}"
                f" {seg['npc_entry']} {radius:.0f}",
                seg.get("wall_timeout", 150))
            qs_after = self.quest_state(q)
            prog = self._go_progress()
            progressed = (prog > last_prog
                          or qs_after["status"] != qs["status"]
                          or qs_after.get("can_complete") or qs_after["rewarded"])
            last_prog = max(last_prog, prog)
            stalls = 0 if progressed else stalls + 1
            log(f"quest {q} useitem attempt {attempt}: {result}"
                f" progress={prog} (stalls {stalls}/{stall_budget})")
            self.ensure_bag_space(seg)
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
        # Vendor runs cross hostile belts like any other transit (live:
        # Grunttwelve died selling, mid-Razormane) -- never start one
        # half-dead; ambient defense fights ambushes but partial-hp
        # 2v1s still lose.
        self.wait_for_health(0.9)
        self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=60.0)
        ok = False
        for attempt in range(seg.get("attempts", 3)):
            result = self.issue_and_wait(
                f"guidestartselljunk {self.char} {seg['vendor']} "
                f"{seg['x']:.1f} {seg['y']:.1f} {seg['z']:.1f}", 300)
            g = self.guide_status()
            if result == "finished" or g.get("gray_items", 1) == 0:
                ok = True
                break
            log(f"sell attempt {attempt + 1}: {result}, grayItems={g.get('gray_items')}")
            if self.at_wrong_layer(seg["x"], seg["y"], seg["z"]):
                log("sell: layer trap at vendor (tent roof) -- unsticking")
                self.unstick(seg)
        # Repair alongside every vendor stop: deaths bleed durability,
        # and at zero the item stops existing statistically -- observed
        # live as a self-reinforcing decay spiral (max hp 259 -> 184,
        # weapon at durability 0 = fighting bare-fisted -> more deaths
        # -> more durability loss). NOTE the repair NPC is usually NOT
        # the junk vendor: repair needs UNIT_NPC_FLAG_REPAIR (0x1000)
        # and e.g. Jark (general goods) silently no-ops.
        rep = seg.get("repair") or self.route.get("repair")
        if rep:
            if self.walk_toward(rep["x"], rep["y"], rep["z"], arrive_within=4.0):
                out = self.ap(f"repair {self.char} {rep['vendor']}")
                m = re.search(r"money before=(\d+), money after=(\d+)", out)
                if m and m.group(1) != m.group(2):
                    log(f"repaired ({int(m.group(1)) - int(m.group(2))} copper)")
        # Gear check on every vendor stop (the gear-floor fix): buy the
        # best usable weapon/armor per slot the bot can afford, then
        # equip. Money-gated inside the engine; a broke bot no-ops.
        self.gear_stop()
        return ok

    def gear_stop(self) -> None:
        # Home (general goods) vendor first: gear no-ops there but the
        # consumables top-up (food + drink) is what keeps casters from
        # fighting empty (buyupgrades buys both since the mana fix).
        # TRAIN FIRST (training economics, 2026-07-06): spells beat
        # gear at these levels and the learnspell loop stops when money
        # runs out -- shopping before training left casters without
        # their pushback-proof instants. Class training on every town trip (found 2026-07-06: NO
        # generated route had a train segment -- a level-7 mage was
        # fighting with rank-1 Fireball, having never once visited a
        # trainer; the whole fleet was on level-1 abilities).
        tr = self.route.get("class_trainer")
        if tr and self.walk_toward(tr["x"], tr["y"], tr["z"], arrive_within=6.0):
            learned = 0
            for _ in range(30):
                out = self.ap(f"learnspell {self.char} {tr['trainer']}")
                if "no spell" in out:
                    break
                m = re.search(r"has spell after=(\w+)", out)
                if m and m.group(1) == "true":
                    learned += 1
                    time.sleep(1.0)
                else:
                    break  # can't pay / can't learn -- stop looping
            if learned:
                log(f"trained {learned} spell(s) at trainer {tr['trainer']}")


        stops = list(self.route.get("gear_vendors", []))
        hv = self.route.get("home_vendor")
        if hv and hv.get("vendor"):
            stops.insert(0, hv)
        for gv in stops:
            if not self.walk_toward(gv["x"], gv["y"], gv["z"], arrive_within=4.0):
                continue
            out = self.ap(f"buyupgrades {self.char} {gv['vendor']}")
            m = re.search(r"bought=(\d+), consumables=(\d+), equipped=(\d+)", out)
            if m and (m.group(1) != "0" or m.group(2) != "0"):
                log(f"gear: bought {m.group(1)}, consumables {m.group(2)},"
                    f" equipped {m.group(3)} at vendor {gv['vendor']}")
    def seg_train(self, seg: dict) -> bool:
        self.walk_toward(seg["x"], seg["y"], seg["z"], arrive_within=10.0)
        if self.at_wrong_layer(seg["x"], seg["y"], seg["z"]):
            log("train: layer trap at trainer -- unsticking")
            self.unstick(seg)
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

    def grind_camp_for_level(self, seg: dict, lvl: int) -> dict:
        """Grind at the camp of the bot's OWN tier, not the target's.

        Rungs are authored with mobs matched to their `level`: a bot that
        enters grind-to-7 at level 5 camping the rung's own (6-7) mobs
        fights orange at gear floor -- overnight live: Tanktwelve spent
        2.5h/8 deaths at level 5 vs Prairie Wolves, Elwynn's cohort the
        same vs Mangy Wolves. Pick the route's highest grind rung whose
        level <= current bot level (its mobs are green/yellow by
        construction), climbing camps as the bot levels."""
        def condemned(s: dict) -> bool:
            # GRIND-tagged spots (this camp's OWN death budget was
            # explicitly exceeded) ban it immediately, as before -- a
            # single, deliberate, high-confidence signal.
            #
            # Untagged spots (any death, any segment -- see
            # _recover_from_death) are a real hazard signal too, but a
            # single one must NOT ban a camp outright: quest-failure
            # spots share ground with perfectly good neighboring mob
            # fields in dense zones (live: Fizzlewick's wendigo-quest
            # condemnation overlapped the Kharanos boar AND trogg camps
            # from one bad spot, rejecting every safe rung and falling
            # through to an untested, WORSE 8-9 Frostmane camp). Require
            # 2+ nearby before an untagged hazard blocks grind selection
            # -- still catches a real repeat offender (live 2026-07-08:
            # Fizzlewick/Dwarftwelve/Grunttwelve each died 4-9 times to
            # the SAME dense camp across scattered segments whose own
            # individual budgets never tripped) without the single-flaky-
            # death false-positive risk that caused the incident above.
            untagged_nearby = 0
            for spot in getattr(self, "hard_spots", []):
                hx, hy, hlvl = spot[0], spot[1], spot[2]
                if lvl > hlvl or (s["x"] - hx) ** 2 + (s["y"] - hy) ** 2 > 250.0 ** 2:
                    continue
                if len(spot) >= 4 and spot[3] == "grind":
                    return True
                untagged_nearby += 1
                if untagged_nearby >= 2:
                    return True
            return False

        # Falling back past a NEARBY condemned tier onto a distant "safe"
        # one can land on content so far under the bot's level it's gray --
        # no real XP, no valid targets, permanently. Live 2026-07-08:
        # Roguetwelve (level 9, running grind-to-10) had both its 8 and 10
        # tiers condemned at once (they sit only ~144yd apart, so one
        # hazard cluster condemns both) and fell all the way back to its
        # level-6 tier -- entry 1509, real level 2-3 -- candidates=0, the
        # grind cycle failed twice, and the segment's own time budget ran
        # out with zero progress. A "safe" camp that can never finish the
        # grind isn't a fallback, it's a different way to get stuck.
        #
        # Prefer the highest uncondemned tier within 3 rungs of the segment
        # actually being run (still worth fighting, by construction -- each
        # rung is authored/mined to be appropriate a couple tiers below the
        # one above it). Only when EVERY nearby tier is condemned does a
        # condemned-but-appropriately-leveled one become preferable to a
        # guaranteed-gray "safe" one -- some hazard risk beats a permanent
        # stall, matching this project's own priority: real progress over
        # relaxing/deferring around a problem.
        eligible = [s for s in self.route["segments"]
                    if s.get("type") == "grind_to_level" and "entry" in s
                    and s["level"] <= lvl]
        nearby = [s for s in eligible if seg["level"] - s["level"] <= 3]
        viable = [s for s in nearby if not condemned(s)]
        if viable:
            return max(viable, key=lambda s: s["level"])
        if nearby:
            return max(nearby, key=lambda s: s["level"])
        safe = [s for s in eligible if not condemned(s)]
        if safe:
            return max(safe, key=lambda s: s["level"])
        if eligible:
            return max(eligible, key=lambda s: s["level"])
        return seg

    def seg_grind_to_level(self, seg: dict) -> bool:
        target = seg["level"]
        consecutive_failures = 0
        cycle = 0
        deadline = time.time() + seg.get("max_minutes", 240) * 60
        while time.time() < deadline:
            # Grinds are the leveling backbone on (chosen) green mobs, so
            # a higher budget than quests: keep working respawns rather
            # than skip the level. But still bounded -- a grind anchor
            # that pulls a deadly pack shouldn't loop to 100 deaths.
            self.check_death_budget(seg, default=12)
            lvl = self.level()
            if lvl >= target:
                log(f"grind_to_level {target}: reached (level {lvl})")
                return True
            unlocked = [g for g in getattr(self, "pass_level_gates", []) if g <= lvl]
            if unlocked:
                log(f"grind_to_level {target}: level {lvl} unlocks deferred quest(s)"
                    f" (min_level {min(unlocked)}) -- yielding to quest XP")
                raise GrindYield()
            # Review loot each cycle: equip upgrades, then vendor junk.
            self.equip_upgrades()
            self.ensure_bag_space(seg)
            camp = self.grind_camp_for_level(seg, lvl)
            if camp is not seg and camp.get("level") != getattr(self, "_last_camp_tier", None):
                self._last_camp_tier = camp.get("level")
                log(f"grind_to_level {target}: at level {lvl}, farming the "
                    f"tier-{camp['level']} camp (entry {camp['entry']})")
            points = camp.get("points") or [[camp["x"], camp["y"], camp["z"]]]
            anchor = points[cycle % len(points)]
            cycle += 1
            st = self.bot_status()
            dist2 = (st.get("x", 1e9) - anchor[0]) ** 2 + (st.get("y", 1e9) - anchor[1]) ** 2
            # Walk threshold MUST be inside the guide's 50yd target-
            # search radius: a bot parked 50-79yd from the anchor found
            # zero candidates forever (live: Roguetwelve at 68yd,
            # operationTicks 51, candidates=0 -- fleet-wide level
            # freeze). Anchor rotation makes this bite every cycle.
            if dist2 > 30.0 ** 2:
                if not self.walk_toward(anchor[0], anchor[1], anchor[2], arrive_within=15.0):
                    self.unstick(camp if camp.get("unstick") else seg)
            spell = seg.get("spell", self.route.get("opportunistic_spell", 0))
            self.wait_for_health()
            # Gate 3 route-quality mitigation: issue exactly one kill at a
            # time until GuideRuntime enforces readiness between every pull.
            # Route-authored kills_per_issue values are deliberately ignored;
            # an external health check before a chained guide is not enough.
            heal = seg.get("heal_spell", self.route.get("heal_spell", 0))
            result = self.issue_and_wait(
                f"guidestartgrind {self.char} {camp['entry']} "
                f"1 {spell} {heal}",
                seg.get("cycle_timeout", 480))
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

    def _decision(self, *, seg: dict, reason: str, blocked: dict | None = None,
                  guide_quest_available: bool | None = None,
                  grind_reason: str | None = None) -> None:
        """Emit ONE machine-parseable record of a routing decision.

        The whole point of this line is to answer, after the fact and
        without guessing, "why did this bot do that -- and in particular
        why is it grinding." The prose `log()` lines around the pass loop
        are for a human reading a tail; this is the stable, greppable form
        route_metrics.py consumes (prefix `DECISION`, space-separated
        key=value, no spaces inside values). Keep the keys stable.
        """
        qs = self.quest_state(788)  # one read: level + xp for this record
        need = XP_TO_NEXT.get(qs["level"])
        xp_pct = max(0, min(99, int(100 * qs["xp"] / need))) if need else -1
        parts = [
            "DECISION",
            f"char={self.char}",
            f"family={self.family}",
            f"level={qs['level']}",
            f"xp_pct={xp_pct}",
            f"seg={seg.get('id', '?')}",
            f"type={seg.get('type', '?')}",
            f"hub={seg.get('hub', '-')}",
            f"guide_step={seg.get('guide_step', '-')}",
            f"reason={reason}",
        ]
        if guide_quest_available is not None:
            parts.append(f"guide_quest_available={int(guide_quest_available)}")
        if grind_reason is not None:
            parts.append(f"grind_reason={grind_reason}")
        if blocked:
            # e.g. blocked=min_level:3,hazard:1,prereq:2,death_budget:0
            parts.append("blocked=" + ",".join(
                f"{k}:{v}" for k, v in blocked.items()))
        log(" ".join(parts))

    def _execute_grind(self, seg: dict, deferred: list) -> bool:
        """Run one grind rung as the pass FALLBACK (called only when the
        quest sweep made no progress). The rung is already in `deferred`
        (held there during the sweep); on reaching its target level it's
        promoted to done and dropped from deferred, otherwise it stays
        deferred for a later pass. Returns True on real progress (the bot
        leveled or the rung completed). Mirrors the grind-specific handling
        that used to live inline in the sweep's except blocks -- a grind is
        NEVER skipped (it is the leveling ladder); a deadly camp is condemned
        so tier selection drops to a lower rung, and the grind re-queues."""
        sid = seg["id"]
        self.current_seg = seg
        self.seg_death_baseline = self.state["deaths"]
        self.check_alive_or_recover()
        self.ensure_bag_space(seg)
        before = self.level()
        try:
            ok = self.seg_grind_to_level(seg)
        except GrindYield:
            self.record_level()
            return self.level() > before
        except SegmentAbandoned as exc:
            lvl = self.level()
            camp = self.grind_camp_for_level(seg, lvl)
            self.hard_spots.append([camp["x"], camp["y"], lvl, "grind"])
            self.save_state()
            self.seg_death_baseline = self.state["deaths"]
            log(f"[{sid}] camp ({camp['x']:.0f},{camp['y']:.0f}) condemned at "
                f"level {lvl} ({exc}) -- grind continues at a lower-tier camp")
            self.record_level()
            return self.level() > before
        self.record_level()
        if ok:
            self.state["done"].append(sid)
            if seg in deferred:
                deferred.remove(seg)
            self.save_state()
            log(f"[{sid}] DONE (level {self.level()})")
            return True
        return self.level() > before

    def run(self, start_at: str | None = None) -> int:
        handlers = {
            "quest_grind": self.seg_quest_grind,
            "quest_gameobject": self.seg_quest_gameobject,
            "quest_useitem_unit": self.seg_quest_useitem_unit,
            "quest_delivery": self.seg_quest_delivery,
            "quest_accept": self.seg_quest_accept,
            "quest_turnin": self.seg_quest_turnin,
            "walk": self.seg_walk,
            "sell": self.seg_sell,
            "train": self.seg_train,
            "grind_to_level": self.seg_grind_to_level,
        }
        self.ensure_online()
        self.record_level()
        # One gear stop up front (level-4+ bots with vendors): waiting for
        # bag pressure meant a level-7 mage kept fighting in level-1
        # whites for hours after the buy path shipped.
        if self.route.get("gear_vendors") and self.level() >= 4:
            self.gear_stop()
        started = start_at is None
        # Segments skipped for min_level/prereq are DEFERRED, not
        # dropped: later grind segments raise the level (and later
        # quests satisfy prereqs), so unmet gates get follow-up passes.
        # Found live: a skip-forever semantic silently dropped a
        # prerequisite quest and wedged its whole chain.
        queue = list(self.route["segments"])
        # Quests are MANDATORY -- a segment is only ever DEFERRED, never
        # silently skipped. Unmet prereq, below min_level, or "too hard
        # at this level" (death budget) all re-queue it for a later pass
        # after grinds/other quests raise the level or satisfy the
        # prereq. Genuine impossibility (wrong class/race) is filtered at
        # authoring time. If a whole pass makes NO progress and segments
        # remain, they are surfaced for intervention -- a hard quest gets
        # fixed, not dropped.
        # Versioned skip/gate state. The skipped list, defer-fail counts, and
        # relevel gates are memory of what DIDN'T work -- but that memory is
        # only valid for the route that produced it. Routes get regenerated
        # (better Zygor coords, added quests, moved camps); a skip earned under
        # an old, worse route silently makes the current route look worse than
        # it is (live 2026-07-08: q834 stayed skipped from a pre-regeneration
        # run though it was no longer even in the route, and 269 stale skips
        # fleet-wide were forcing grind by hiding quests that may now work).
        # Hash the current segments; if it differs from what this state was
        # last gated against, wipe the stale gating so every quest gets a fair
        # re-attempt on the new route. Live-learned hazards (hard_spots) are
        # location truth, not route-derived, so they persist across versions.
        import hashlib
        rv = hashlib.sha1(
            json.dumps(self.route["segments"], sort_keys=True).encode()
        ).hexdigest()[:12]
        if self.state.get("route_version") != rv:
            n_skip = len(self.state.get("skipped", []))
            self.state["skipped"] = []
            self.state["defer_fails"] = {}
            self.state["relevel_gate"] = {}
            self.state["route_version"] = rv
            self.save_state()
            log(f"route changed (version {rv}); cleared stale gating -- "
                f"{n_skip} skipped quest(s) re-opened for a fresh attempt")
        # Persisted (not just in-memory): every fleet relaunch was wiping
        # these, so Magetwelve re-burned a fresh death budget at the same
        # gated Fargodeep mine after each relaunch -- three times in one
        # night. Gates expire naturally when the bot outlevels them.
        self.relevel_gate = self.state.setdefault("relevel_gate", {})
        # Migration: grind rungs must never carry relevel gates (they ARE
        # the ladder) -- strip any saved before the never-gate-grinds rule
        # (live: Baldrick stayed deadlocked by a persisted grind gate).
        for k in [k for k in self.relevel_gate if k.startswith("grind-")]:
            del self.relevel_gate[k]
        self.state["skipped"] = [x for x in self.state.get("skipped", [])
                                 if not x.startswith("grind-")]
        for k in [k for k in self.state.get("defer_fails", {})
                  if k.startswith("grind-")]:
            del self.state["defer_fails"][k]
        self.hard_spots = self.state.setdefault("hard_spots", [])
        self.state.setdefault("skipped", [])
        self.state.setdefault("defer_fails", {})
        # Quest ids that other segments depend on (chain-starters). Skipping one
        # of these cascade-destroys its whole chain (found live: q376 failed on a
        # bad kill coord, got skipped, and took the entire Tirisfal chain --
        # q3902/q380/q381/q382/q383/q6395 -- down with it, stranding the bot).
        # These are NEVER permanently skipped -- only deferred/retried -- so a
        # transient/coord failure can't wipe a chain. The grind ladder carries
        # leveling meanwhile, and a coord repair lets the chain resume.
        self.prereq_quests = {s.get("requires_quest") for s in self.route["segments"]
                              if s.get("requires_quest")}
        level_seen = self.level()
        # Grind now runs at most ONE rung per pass (fallback phase), so a
        # route that must climb several rungs to unlock its late quests needs
        # more passes than when grinds ran inline. Each idle pass is cheap
        # (it only re-checks gates); the ceiling just prevents a true wedge
        # from looping forever. 40 comfortably covers ~6 rungs + quest waves.
        max_passes = 40
        complete = False
        for pass_no in range(max_passes):
            deferred = []
            completed_any = False
            # min_level values of segments deferred this pass -- read by
            # seg_grind_to_level so a long grind yields the moment it
            # levels past one of these gates (see GrindYield).
            self.pass_level_gates = []
            # GRIND IS A FALLBACK, NOT A PEER OF QUESTS. Grind rungs are held
            # OUT of the quest sweep and run only after it, and only if no
            # quest made progress this pass (see the fallback phase below).
            # Before this, grinds were interleaved into the queue by level
            # and ran the moment they were reached -- so a bot could grind a
            # rung while a workable quest sat later in the same pass, and the
            # logs made it look like grinding was the plan rather than the
            # last resort. Holding them here makes "grind only when blocked"
            # true at runtime and makes the decision log say so.
            grinds_this_pass = []
            # Why each quest was NOT runnable this pass -- surfaced on the
            # fallback grind's DECISION record so it's obvious WHAT blocked
            # the bot into grinding (min_level gate, hazard, prereq, or a
            # death-budget relevel gate).
            blocked_tally = {"min_level": 0, "hazard": 0,
                             "prereq": 0, "death_budget": 0}
            for seg in queue:
                sid = seg["id"]
                if not started:
                    started = sid == start_at
                    if not started:
                        continue
                if sid in self.state["done"]:
                    continue
                if sid in self.state["skipped"]:
                    continue
                if seg.get("type") == "grind_to_level":
                    # Hold for the fallback phase; keep it in the queue.
                    grinds_this_pass.append(seg)
                    deferred.append(seg)
                    continue
                req = seg.get("requires_quest")
                if req and not self.quest_state(req)["rewarded"]:
                    req_segments = [s for s in self.route["segments"]
                                    if s.get("quest") == req]
                    if req_segments and all(s["id"] in self.state["skipped"]
                                            for s in req_segments):
                        self.state["skipped"].append(sid)
                        self.save_state()
                        log(f"[{sid}] prerequisite quest {req} was permanently skipped"
                            " -- cascading skip (not runnable independently)")
                        continue
                    log(f"[{sid}] prerequisite quest {req} not rewarded -- deferring")
                    blocked_tally["prereq"] += 1
                    deferred.append(seg)
                    continue
                min_lvl = seg.get("min_level")
                if min_lvl and self.level() < min_lvl:
                    log(f"[{sid}] below min_level {min_lvl} -- deferring")
                    self.pass_level_gates.append(min_lvl)
                    blocked_tally["min_level"] += 1
                    deferred.append(seg)
                    continue
                gate = self.relevel_gate.get(sid)
                if gate is not None and self.level() <= gate:
                    log(f"[{sid}] too hard at level {gate}; grind higher first -- deferring")
                    blocked_tally["death_budget"] += 1
                    deferred.append(seg)
                    continue
                # Location-aware relevel gate: a spot that spent one
                # segment's death budget is too hard for EVERY combat
                # segment aimed there (live: Magetwelve deferred q60 at
                # Fargodeep after 3 deaths and the route immediately
                # served q47 -- same mine -- for 3 more). 250yd radius:
                # a camp COMPLEX, not a point -- the two Grizzled Den
                # quests' densest clusters sit 360yd apart (cave mouth
                # vs interior) and 100yd let Fizzlewick pay two budgets
                # at one wendigo cave.
                #
                # quest_delivery has no single "x"/"y" (its NPCs are
                # giver_x/y and turnin_x/y) and was never in this check at
                # all -- live 2026-07-08: Nelfhunter/Nelfdruid died 15+/12+
                # times because their giver/turnin coordinates sat inside a
                # real, repeatedly-fatal dense-mob zone that hard_spots was
                # recording fine, but quest_delivery segments never
                # consulted it (a delivery's NPCs are fixed, so "defer and
                # try a different spot" isn't available the way it is for a
                # grind camp -- but "defer and retry once stronger", the
                # same semantics every OTHER gated type here already uses,
                # is exactly what's needed and was simply missing).
                spot = None
                if seg.get("type") in ("quest_grind", "quest_gameobject",
                                       "quest_useitem_unit") and "x" in seg:
                    candidates = [(seg["x"], seg["y"])]
                elif seg.get("type") == "quest_delivery":
                    candidates = [(seg[gx], seg[gy])
                                  for gx, gy in (("giver_x", "giver_y"),
                                                 ("turnin_x", "turnin_y"))
                                  if gx in seg and gy in seg]
                else:
                    candidates = []
                if candidates:
                    lvl_now = self.level()
                    for cx, cy in candidates:
                        for hs in getattr(self, "hard_spots", []):
                            hx, hy, hlvl = hs[0], hs[1], hs[2]
                            if lvl_now <= hlvl and \
                                    (cx - hx) ** 2 + (cy - hy) ** 2 <= 250.0 ** 2:
                                spot = (hx, hy, hlvl)
                                break
                        if spot:
                            break
                if spot:
                    log(f"[{sid}] targets a too-hard spot "
                        f"({spot[0]:.0f},{spot[1]:.0f}, gated at {spot[2]}) -- deferring")
                    blocked_tally["hazard"] += 1
                    deferred.append(seg)
                    continue
                # A quest segment survived every gate -- it runs. This is the
                # "a guide quest WAS runnable, so we did it" case: grind can
                # only ever be reached when this branch fires for nobody.
                self._decision(seg=seg, reason="run_quest",
                               guide_quest_available=True)
                log(f"=== segment [{sid}] ({seg['type']}) ===")
                self.current_seg = seg
                self.seg_death_baseline = self.state["deaths"]
                self.check_alive_or_recover()
                # Global 2-slot rule: clear bag pressure before any
                # segment that could add items (quest/grind/loot/walk
                # deeper). Only quest turn-ins that grant no item are
                # safe to run with full bags.
                if seg.get("type") != "quest_turnin":
                    self.ensure_bag_space(seg)
                try:
                    ok = handlers[seg["type"]](seg)
                except GrindYield:
                    # Not done, not gated: re-queue so it resumes after
                    # the unlocked quests run (they precede it in
                    # `deferred`, preserving queue order).
                    deferred.append(seg)
                    self.record_level()
                    continue
                except SegmentAbandoned as exc:
                    # Too deadly at THIS level -- don't skip; require a
                    # higher level, then retry stronger. The always-
                    # available grind segments raise the level.
                    lvl = self.level()
                    if seg.get("type") == "grind_to_level":
                        # NEVER relevel-gate a grind: it IS the ladder,
                        # so a gated grind with every quest also gated is
                        # a hard deadlock (live: Baldrick exited at 7,
                        # '9 segments STUCK', after the bear camp burned
                        # the grind budget). Condemn the CAMP instead --
                        # tier selection then falls back to a lower
                        # rung's camp -- and re-queue the grind at once.
                        camp = self.grind_camp_for_level(seg, lvl)
                        self.hard_spots.append([camp["x"], camp["y"], lvl, "grind"])
                        self.save_state()
                        self.seg_death_baseline = self.state["deaths"]
                        log(f"[{sid}] camp ({camp['x']:.0f},{camp['y']:.0f}) "
                            f"condemned at level {lvl} ({exc}) -- grind continues"
                            " at a lower-tier camp")
                        deferred.append(seg)
                        self.record_level()
                        continue
                    self.relevel_gate[sid] = lvl
                    if "x" in seg:
                        self.hard_spots.append([seg["x"], seg["y"], lvl])
                        self.save_state()
                    log(f"[{sid}] too hard at level {lvl} ({exc}) -- will grind up and retry (NOT skipping)")
                    deferred.append(seg)
                    self.record_level()
                    continue
                self.record_level()
                if ok:
                    completed_any = True
                    self.relevel_gate.pop(sid, None)
                    self.state["done"].append(sid)
                    self.save_state()
                    if seg["type"] in ("quest_grind", "quest_turnin", "train"):
                        # New rewards may beat what's worn -- equip them
                        # (idempotent; engine validates; counted for real).
                        out = self.ap(f"equipupgrades {self.char}")
                        m = re.search(r"Equipped (\d+) upgrade", out)
                        if m and m.group(1) != "0":
                            log(f"equipped {m.group(1)} upgrade(s)")
                    log(f"[{sid}] DONE (level {self.level()})")
                elif seg.get("optional"):
                    log(f"[{sid}] FAILED but optional -- continuing")
                    self.state["done"].append(sid)
                    self.save_state()
                else:
                    # Non-death failure (stalls / never-rewarded). Defer for a
                    # retry, but count it: a quest that fails its real attempts
                    # DEFER_FAIL_LIMIT times is undoable at any level (unsupported
                    # behavior, unreachable giver, phased/event) -- permanently
                    # skip it so the bot stops re-failing it every pass and gets
                    # on with doable quests + the grind fallback.
                    if seg.get("type") == "grind_to_level":
                        # Grinds are NEVER skip-eligible: a grind that
                        # returns False (deadline/budget churn) is not an
                        # undoable quest, it is the leveling ladder --
                        # permanently skipping one deadlocked Fizzlewick
                        # at 7 with every quest also gated (live,
                        # 2026-07-06, third wedge of the day). Re-queue.
                        deferred.append(seg)
                        continue
                    n = self.state["defer_fails"].get(sid, 0) + 1
                    self.state["defer_fails"][sid] = n
                    self.save_state()  # persist so a restart doesn't reset skip progress
                    is_prereq = seg.get("quest") in self.prereq_quests
                    limit = PREREQ_FAIL_LIMIT if is_prereq else DEFER_FAIL_LIMIT
                    if n >= limit:
                        self.state["skipped"].append(sid)
                        self.save_state()
                        tag = ("CHAIN PREREQUISITE undoable after retries -- skipping "
                               "(chain lost; grind ladder carries leveling)" if is_prereq
                               else "blacklisted -- undoable; grind/other quests carry leveling")
                        log(f"[{sid}] PERMANENTLY SKIPPED after {n} failed attempts ({tag})")
                    else:
                        kind = "CHAIN PREREQUISITE" if is_prereq else "quest"
                        log(f"[{sid}] failed ({kind}, fail {n}/{limit}) -- deferring for retry")
                        deferred.append(seg)
            # ---- FALLBACK PHASE: grind ONLY because the sweep couldn't ----
            # The quest sweep above already ran every runnable quest. Reach
            # here having completed none only when quests are either blocked
            # behind a level/hazard gate OR simply exhausted below the route's
            # target level -- both cases want the same lever: grind up. Run
            # ONE rung, the lowest above the current level, so it grinds JUST
            # enough -- its own GrindYield hands control back the instant the
            # bot crosses the next deferred min_level gate, and blocked quests
            # get first crack next pass. If a quest DID complete this pass,
            # skip grinding entirely. This is the "grind is a fallback, not
            # the default path" guarantee, enforced at runtime, not just in
            # the generated ordering.
            if not completed_any and grinds_this_pass:
                blocked_quests = [s for s in deferred
                                  if s.get("type") != "grind_to_level"]
                cur = self.level()
                ups = sorted((g for g in grinds_this_pass if g["level"] > cur),
                             key=lambda g: g["level"])
                if ups:
                    grind = ups[0]
                    if grind.get("guide_step") is not None:
                        grind_reason = "explicit_guide_grind"
                    elif blocked_quests:
                        grind_reason = "no_safe_quest"
                    else:
                        grind_reason = "reach_target_level"
                    self._decision(
                        seg=grind, reason="run_grind",
                        guide_quest_available=False,
                        grind_reason=grind_reason, blocked=blocked_tally)
                    log(f"=== segment [{grind['id']}] (grind_to_level) "
                        f"[FALLBACK reason={grind_reason}: "
                        f"{len(blocked_quests)} quest(s) blocked {blocked_tally}] ===")
                    self._execute_grind(grind, deferred)
            # Route is complete when nothing MEANINGFUL is left: no quest
            # segments deferred, and no grind rung still above the bot's
            # level. Leftover low grind rungs (already out-levelled) are inert
            # scaffolding, not unfinished work -- treating them as undone was
            # a false "STUCK" once quests ran dry above them.
            remaining = [s for s in deferred
                         if s.get("type") != "grind_to_level"
                         or s["level"] > self.level()]
            if not remaining:
                complete = True
                break
            # Progress this pass = something completed OR the bot leveled
            # (unlocking min_level / relevel-gated segments next pass).
            progressed = completed_any or self.level() > level_seen
            level_seen = self.level()
            if not progressed:
                stuck = [s["id"] for s in remaining]
                log(f"no progress this pass; {len(stuck)} segment(s) STUCK at level "
                    f"{self.level()}: {stuck[:8]} -- stopping for intervention "
                    "(a hard quest needs fixing, not skipping)")
                return 1
            queue = deferred
            log(f"--- deferred pass {pass_no + 2}: {len(deferred)} segment(s) ---")
        if not complete and queue:
            log(f"exhausted {max_passes} passes; {len(queue)} undone: "
                f"{[s['id'] for s in queue][:8]} -- stopping for intervention")
            return 1
        log(f"route complete at level {self.level()}; deaths={self.state['deaths']}")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--route", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--start-at", default=None)
    parser.add_argument("--allow-tele", action="store_true",
                        help="emergency-only: permit GM teleport when a walked unstick fails")
    parser.add_argument("--host", default=os.environ.get("AP_SOAP_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("AP_SOAP_PORT", "7878")))
    parser.add_argument("--user", default=os.environ.get("AP_SOAP_USER", ""))
    parser.add_argument("--password", default=os.environ.get("AP_SOAP_PASSWORD", ""))
    args = parser.parse_args()
    if not args.user or not args.password:
        print("missing SOAP credentials", file=sys.stderr)
        return 2
    global ALLOW_TELE
    ALLOW_TELE = args.allow_tele
    with open(args.route) as f:
        route = json.load(f)
    # Family label for decision logs: the route filename without a clone
    # suffix (durotar_orc_warrior_1_12__Korgath.json -> the base family).
    route.setdefault("family", os.path.basename(args.route).split("__")[0]
                     .removesuffix(".json"))
    cfg = Config(host=args.host, port=args.port, user=args.user,
                 password=args.password, bot_account=route["account"],
                 bot_char=route["char"], creature_entry=0)
    return Runner(cfg, route, args.state).run(args.start_at)


if __name__ == "__main__":
    sys.exit(main())
