"""Runtime pass-ordering: grind is a strict fallback, never a peer of quests.

route_runner.py's pass loop is normally live-orchestration only (it drives a
real server over SOAP), so it has no unit coverage. This test stubs every
SOAP/IO touchpoint with a tiny deterministic "world" so the pure control flow
-- the part that decides quest-vs-grind and in what order -- can be asserted
offline. It exists because that ordering was rewritten (grind held out of the
quest sweep, run only as a post-sweep fallback) and ships to the whole fleet.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("route_runner", TOOLS / "route_runner.py")
rr = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(rr)


class SimRunner(rr.Runner):
    """Runner with every server/IO call replaced by an in-memory sim: a quest
    'completes' once the bot's level meets the segment's min_level; a grind
    raises the level to its target (yielding at the lowest pending min_level
    gate, exactly like the real GrindYield)."""

    def __init__(self, route: dict) -> None:
        self.route = route
        self.char = "Sim"
        self.account = "sim"
        self.family = "sim"
        self.state = {"done": [], "level_history": [], "deaths": 0,
                      "segment_attempts": {}, "relevel_gate": {},
                      "skipped": [], "defer_fails": {}, "hard_spots": []}
        self.recovering = False
        self.current_seg = None
        self._safe_path = None
        self._sim_level = 1
        self.rewarded: set[int] = set()
        self.decisions: list[tuple[str, str]] = []

    # --- IO / SOAP stubs -------------------------------------------------
    def ensure_online(self) -> None: pass
    def record_level(self) -> None: pass
    def gear_stop(self) -> None: pass
    def save_state(self) -> None: pass
    def check_alive_or_recover(self) -> bool: return False
    def ensure_bag_space(self, seg) -> None: pass
    def ap(self, cmd: str) -> str: return ""

    def level(self) -> int:
        return self._sim_level

    def quest_state(self, qid: int) -> dict:
        return {"status": 0, "rewarded": qid in self.rewarded,
                "level": self._sim_level, "xp": 0, "canComplete": False}

    def _decision(self, *, seg, reason, blocked=None,
                  guide_quest_available=None, grind_reason=None) -> None:
        self.decisions.append((reason, seg["id"]))

    # --- quest handlers: the sweep already gated on min_level, so any quest
    #     that reaches its handler is runnable -> complete it.
    def _do_quest(self, seg) -> bool:
        if seg.get("quest"):
            self.rewarded.add(seg["quest"])
        return True

    seg_quest_grind = _do_quest
    seg_quest_gameobject = _do_quest
    seg_quest_delivery = _do_quest
    seg_quest_useitem_unit = _do_quest
    seg_quest_accept = _do_quest
    seg_quest_turnin = _do_quest

    def seg_walk(self, seg) -> bool: return True
    def seg_sell(self, seg) -> bool: return True
    def seg_train(self, seg) -> bool: return True

    def seg_grind_to_level(self, seg) -> bool:
        gates = [g for g in getattr(self, "pass_level_gates", [])
                 if g > self._sim_level]
        target = min([seg["level"]] + gates)
        self._sim_level = max(self._sim_level, target)
        if self._sim_level >= seg["level"]:
            return True
        raise rr.GrindYield()

    def grind_camp_for_level(self, seg, lvl):
        return seg


ROUTE = {
    "char": "Sim", "account": "sim",
    "segments": [
        {"id": "q1", "type": "quest_grind", "quest": 1, "min_level": 1},
        {"id": "q2", "type": "quest_grind", "quest": 2, "min_level": 3},
        {"id": "grind-to-3", "type": "grind_to_level", "level": 3,
         "x": 0, "y": 0, "z": 0, "entry": 1},
        {"id": "grind-to-5", "type": "grind_to_level", "level": 5,
         "x": 0, "y": 0, "z": 0, "entry": 1},
    ],
}


class PassFallbackTest(unittest.TestCase):
    def test_grind_is_fallback_only_and_never_precedes_a_runnable_quest(self) -> None:
        sim = SimRunner({k: v for k, v in ROUTE.items()})
        rc = sim.run()

        self.assertEqual(0, rc, "route should complete")
        self.assertEqual(5, sim._sim_level, "should reach the top rung's level")

        kinds = [d[0] for d in sim.decisions]
        # A grind decision never comes before the very first quest decision.
        first_grind = kinds.index("run_grind") if "run_grind" in kinds else None
        first_quest = kinds.index("run_quest")
        self.assertLess(first_quest, first_grind,
                        "a quest must run before any grind")

        # Exact expected order: q1 (runnable at L1) -> grind to unlock q2 ->
        # q2 -> grind to reach target. q2 is never grinded past.
        self.assertEqual(
            [("run_quest", "q1"), ("run_grind", "grind-to-3"),
             ("run_quest", "q2"), ("run_grind", "grind-to-5")],
            sim.decisions)

    def test_no_grind_when_every_quest_is_runnable(self) -> None:
        # Both quests runnable from level 1, and no grind rung is needed to
        # reach the route's target (top rung level 1) -> grind never fires.
        route = {"char": "Sim", "account": "sim", "segments": [
            {"id": "q1", "type": "quest_grind", "quest": 1, "min_level": 1},
            {"id": "q2", "type": "quest_grind", "quest": 2, "min_level": 1},
            {"id": "grind-to-1", "type": "grind_to_level", "level": 1,
             "x": 0, "y": 0, "z": 0, "entry": 1},
        ]}
        sim = SimRunner(route)
        rc = sim.run()
        self.assertEqual(0, rc)
        self.assertEqual([("run_quest", "q1"), ("run_quest", "q2")],
                         sim.decisions)
        self.assertNotIn("run_grind", [d[0] for d in sim.decisions])


if __name__ == "__main__":
    unittest.main()
