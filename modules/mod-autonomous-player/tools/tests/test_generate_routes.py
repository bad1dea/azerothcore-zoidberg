from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest


TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("generate_routes", TOOLS / "generate_routes.py")
routes = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(routes)


def kill_quest(qid: int, *, mob_level: int = 2, previous: int = 0) -> dict:
    return {
        "quest": qid,
        "title": f"Quest {qid}",
        "reason": "route",
        "behaviors": ["kill"],
        "min_level": 1,
        "quest_level": mob_level,
        "chain": {"previous": previous},
        "starters": [{"entry": 10, "x": 1, "y": 2, "z": 3}],
        "enders": [{"entry": 11, "x": 4, "y": 5, "z": 6}],
        "objectives": [{
            "type": "kill", "entry": 100 + qid,
            "local_sources": [{
                "entry": 100 + qid, "x": 8, "y": 9, "z": 10,
                "minlevel": mob_level, "maxlevel": mob_level, "rank": 0,
            }],
        }],
    }


class GenerateRoutesTest(unittest.TestCase):
    def test_item_collection_prefers_easiest_source_over_denser_high_level_source(self) -> None:
        objective = {
            "type": "item",
            "local_sources": [
                {"kind": "creature", "entry": 200, "spawns": [
                    {"x": x, "y": 0, "z": 0, "maxlevel": 8, "rank": 0}
                    for x in (0, 1, 2, 3)
                ]},
                {"kind": "creature", "entry": 100, "spawns": [
                    {"x": 100, "y": 0, "z": 0, "maxlevel": 5, "rank": 0}
                ]},
            ],
        }
        self.assertEqual(100, routes.objective_target(objective, None)[1])

    def test_combat_gate_uses_mob_level_but_allows_first_starter_fight(self) -> None:
        existing = {"segments": []}
        starter = routes.make_segment(kill_quest(1, mob_level=2), 10, existing)
        hard = routes.make_segment(kill_quest(2, mob_level=8), 10, existing)
        self.assertEqual(1, starter["min_level"])
        # A 1-level deficit is allowed (generalizing the level<=2 starter-mob
        # exception to every mob level): a level-7 bot may now fight a
        # level-8 mob, not just an exact-level one.
        self.assertEqual(7, hard["min_level"])

    def test_build_drops_blocked_chain_and_reuses_authored_navigation_and_grind(self) -> None:
        existing = {
            "char": "Test", "account": "test", "home_vendor": {},
            "segments": [
                {"id": "old-q1", "type": "quest_grind", "quest": 1,
                 "unstick": "SafeHub", "giver_via": [1, 1, 1]},
                {"id": "old-grind", "type": "grind_to_level", "level": 4,
                 "entry": 77, "x": 7, "y": 8, "z": 9, "unstick": "SafeHub"},
            ],
        }
        variant = {"quests": [kill_quest(1), kill_quest(2, previous=99)]}
        route, stats = routes.build_route(existing, variant, 4)
        quest_segments = [s for s in route["segments"] if s.get("quest")]
        self.assertEqual([1], [s["quest"] for s in quest_segments])
        self.assertEqual("SafeHub", quest_segments[0]["unstick"])
        self.assertEqual([1, 1, 1], quest_segments[0]["giver_via"])
        grind = next(s for s in route["segments"] if s["type"] == "grind_to_level")
        self.assertEqual((4, 77, "SafeHub"),
                         (grind["level"], grind["entry"], grind["unstick"]))
        self.assertEqual(1, stats["dropped"]["missing_prerequisite"])

    def test_committed_outputs_have_resolvable_prereqs_and_authored_grinds(self) -> None:
        config = json.loads((TOOLS / "coverage_families.json").read_text())
        for family in config["families"]:
            for variant in family["variants"]:
                name = variant["route"]
                generated = json.loads((TOOLS / "routes_generated" / name).read_text())
                authored = json.loads((TOOLS / "routes" / name).read_text())
                quest_ids = {s["quest"] for s in generated["segments"] if s.get("quest")}
                for seg in generated["segments"]:
                    if seg.get("requires_quest"):
                        self.assertIn(seg["requires_quest"], quest_ids, name)
                expected = {(s["level"], s["entry"], s["x"], s["y"], s["z"])
                            for s in authored["segments"]
                            if s.get("type") == "grind_to_level"}
                if not expected:
                    # A brand-new family (no hand-authored baseline yet, e.g.
                    # the Zygor-rollout Teldrassil/Ammen Vale routes) has no
                    # grind rungs to reuse -- build_route's own "defensive
                    # fallback" branch invents them from mined kill spawns
                    # instead. Nothing to compare here; every OTHER family
                    # still enforces the strict reuse-exactly-what's-
                    # authored check below.
                    continue
                actual = {(s["level"], s["entry"], s["x"], s["y"], s["z"])
                          for s in generated["segments"]
                          if s.get("type") == "grind_to_level"}
                self.assertEqual(expected, actual, name)


if __name__ == "__main__":
    unittest.main()
