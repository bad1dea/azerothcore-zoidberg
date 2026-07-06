import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

import safe_path  # noqa: E402


class SafePathTest(unittest.TestCase):
    def setUp(self) -> None:
        self._saved = safe_path._spawns_cache

    def tearDown(self) -> None:
        safe_path._spawns_cache = self._saved

    def test_detours_around_synthetic_camp(self) -> None:
        # A dense camp squarely on the straight line; a clear lane above.
        camp = [[x, 0.0, 5.0, 8] for x in range(100, 401, 15)
                for _ in range(2)]
        safe_path._spawns_cache = {"9": camp}
        hops = safe_path.plan(9, 0.0, 0.0, 500.0, 0.0, 6)
        self.assertIsNotNone(hops)
        direct = safe_path.path_threat(9, [(0, 0), (500, 0)], 6)
        planned = safe_path.path_threat(
            9, [(0, 0)] + [(h[0], h[1]) for h in hops] + [(500, 0)], 6)
        self.assertLess(planned, direct * 0.35,
                        f"planned={planned} direct={direct}")

    def test_gray_mobs_are_ignored(self) -> None:
        # Level-1 critters vs a level-8 bot: no threat, no plan needed.
        safe_path._spawns_cache = {"9": [[250.0, 0.0, 5.0, 1]]}
        self.assertIsNone(safe_path.plan(9, 0.0, 0.0, 500.0, 0.0, 8))

    def test_real_corridors_reduce_threat(self) -> None:
        safe_path._spawns_cache = None  # load the committed snapshot
        for map_id, s, d in (
                (0, (-9459.5, 42.1), (-9722.0, 718.0)),   # Goldshire->wolves
                (1, (317.8, -4734.0), (-64.1, -4982.2))):  # RazorHill->keep
            hops = safe_path.plan(map_id, s[0], s[1], d[0], d[1], 6)
            self.assertIsNotNone(hops, f"map {map_id}: no plan")
            direct = safe_path.path_threat(map_id, [s, d], 6)
            planned = safe_path.path_threat(
                map_id, [s] + [(h[0], h[1]) for h in hops] + [d], 6)
            self.assertLessEqual(planned, direct,
                                 f"map {map_id}: {planned} > {direct}")


if __name__ == "__main__":
    unittest.main()
