from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import unittest


TOOLS = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).with_name("fixtures")
SPEC = importlib.util.spec_from_file_location("coverage", TOOLS / "compile_route_coverage.py")
coverage = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(coverage)


class CoverageCompilerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.snapshot = json.loads((FIXTURES / "snapshot.json").read_text())
        self.route = json.loads((FIXTURES / "route.json").read_text())
        self.profile = coverage.parse_profile(FIXTURES / "profile.xml")
        self.family = {"id": "fixture", "map": 0, "bounds": [0, 100, 0, 100]}
        self.variant = {"route": "route.json", "race_mask": 1, "class_mask": 1}

    def compile(self) -> dict:
        return coverage.compile_variant(
            self.family, self.variant, self.route, self.profile,
            self.snapshot, coverage.index_snapshot(self.snapshot),
        )

    def test_extracts_profile_actions_objectives_and_vendor_leads(self) -> None:
        self.assertEqual([100], self.profile["quest_ids"])
        self.assertEqual("KillMob", self.profile["definitions"][0]["objectives"][0]["type"])
        self.assertIn({"action": "GrindTo", "level": 4}, self.profile["actions"])
        self.assertEqual(900, self.profile["vendors"][0]["entry_lead"])

    def test_emits_local_coordinates_chain_and_stale_contradiction(self) -> None:
        compiled = self.compile()
        quest = compiled["quests"][0]
        self.assertEqual("included", quest["status"])
        self.assertEqual(99, quest["chain"]["previous"])
        self.assertEqual(10, quest["starters"][0]["x"])
        self.assertEqual(16, quest["objectives"][0]["local_sources"][0]["x"])
        self.assertEqual([100], compiled["summary"]["stale_comment_contradictions"])

    def test_structured_omission_is_deterministic(self) -> None:
        self.route["segments"] = []
        first = self.compile()
        second = self.compile()
        self.assertEqual(first, second)
        self.assertEqual("deliberate_route_quality_choice", first["quests"][0]["reason"])

    def test_race_and_class_validation(self) -> None:
        self.route["segments"] = []
        self.variant = {"route": "route.json", "race_mask": 2, "class_mask": 2}
        quest = self.compile()["quests"][0]
        self.assertEqual("wrong_race", quest["reason"])
        self.assertEqual(["wrong_race", "wrong_class"], quest["all_reasons"])


if __name__ == "__main__":
    unittest.main()
