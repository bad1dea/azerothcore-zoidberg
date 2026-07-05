#!/usr/bin/env python3
"""Generate quest-first fleet routes from the validated coverage compiler output.

Phase 4 of the Gate 3 route-quality program. The coverage compiler
(`compile_route_coverage.py`) already turns the external Likon69/Honorbuddy
starter profiles into a locally validated, `acore_world`-authoritative quest set
per family/variant -- every id, giver, ender, objective spawn, coordinate, map,
race/class gate, and elite rank comes from the local DB snapshot, never from the
external XML (which is a research lead only). This tool consumes that output and
emits a `route_runner.py` route per variant that is quest-driven rather than
grind-driven:

  * every eligible, locally supported quest in the variant's level band is
    woven in, ordered by level then quest chain;
  * kill / collection quests become `quest_grind` (multi-objective ->
    `kill_entries`), deliveries become `quest_accept` + `quest_turnin`,
    gameobject-collection quests become the new `quest_gameobject` segment
    (live-verified: q3902 Scavenging Deathknell -> REWARDED via Equipment Boxes);
  * elite / group / wrong-level / unreachable-source quests are dropped with a
    recorded reason -- solo bots never attempt them;
  * grinding is a bounded top-off only: a `grind_to_level` bridge is emitted
    solely when the quest plan leaves a level gap the quests themselves cannot
    cover, and only up to the family's exit level. The goal is to finish off a
    level, never to grind a whole one.

The header of each existing route (char, account, opportunistic_spell,
home_vendor, map, unstick, repair) is preserved -- only the segment list is
regenerated -- so the fleet keeps its identity, vendor, and class rotation.

Deterministic: identical coverage + config + existing-route headers produce
identical routes. No DB or live-server access.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


# Behaviors the GuideRuntime can actually execute end to end today. Kept in sync
# with compile_route_coverage.py's SUPPORTED_OBJECTIVES; use_item / exploration
# are deliberately excluded until their steps are live-driven.
SUPPORTED = {"delivery", "kill", "creature_collection", "gameobject"}

# A quest is eligible to be woven in if it is either already in the old route
# ("route") or a locally validated supported quest the old author simply left
# out ("deliberate_route_quality_choice"). Every other omission reason
# (wrong_class, invalid_ender, invalid_local_source, unsupported_objective_
# behavior, etc.) is a real disqualification we honor.
ELIGIBLE_REASONS = {"route", "deliberate_route_quality_choice"}


def target_level(route_name: str) -> int:
    m = re.search(r"_(\d+)_(\d+)\.json$", route_name)
    return int(m.group(2)) if m else 12


def _densest(points: list[tuple], ref: dict | None):
    """From candidate spawn points ``(kind, entry, x, y, z)``, pick the one in
    the densest same-entry cluster (most neighbours of the same entry within
    30yd), breaking ties by nearest to ``ref`` (the giver). A leveling bot kills
    far faster in a real camp than at a lone spawn, and the guide's selection
    radius is small -- so hotspot DENSITY, not proximity to the giver, is what
    keeps ``candidates>0`` and the grind moving. This directly fixes the
    ``candidates=0`` stalls seen when the nearest-to-giver spawn was a straggler
    while the actual camp sat 70+yd away."""
    if not points:
        return None

    def density(p: tuple) -> int:
        _, e, x, y, _ = p
        return sum(1 for (_, e2, x2, y2, _) in points
                   if e2 == e and (x2 - x) ** 2 + (y2 - y) ** 2 <= 30 * 30)

    def refd(p: tuple) -> float:
        _, _, x, y, _ = p
        return ((x - ref["x"]) ** 2 + (y - ref["y"]) ** 2) if ref else 0.0

    return max(points, key=lambda p: (density(p), -refd(p)))


def objective_target(obj: dict, ref: dict | None):
    """Normalize one objective (schemas differ by type) into the solo-safest,
    densest action target:

      * ('kill'|'go', entry, x, y, z) -- a non-elite creature to kill / world
        object to use, chosen from the densest same-entry cluster;
      * "provided"  -- an item handed over at accept, no action needed;
      * None        -- elite-only, or no local source: quest not solo-doable.

    `kill` objectives carry a creature `entry` with flat-coordinate sources;
    `item` (collection) objectives carry an `item` whose sources are creatures
    or objects with nested `spawns`; `gameobject` objectives carry a GO `entry`
    with flat-coordinate sources."""
    t = obj.get("type")
    if t == "gameobject":
        pts = [("go", obj["entry"], s["x"], s["y"], s["z"]) for s in obj.get("local_sources", [])]
        return _densest(pts, ref)
    if t == "kill":
        pts = [("kill", obj["entry"], s["x"], s["y"], s["z"])
               for s in obj.get("local_sources", []) if int(s.get("rank", 0)) == 0]
        return _densest(pts, ref)
    if t == "item":
        if obj.get("provided_at_accept"):
            return "provided"
        pts = []
        for src in obj.get("local_sources", []):
            kind = "go" if src.get("kind") == "gameobject" else "kill"
            for sp in src.get("spawns", []):
                if int(sp.get("rank", 0)) == 0:
                    pts.append((kind, src["entry"], sp["x"], sp["y"], sp["z"]))
        return _densest(pts, ref)
    return None


def quest_is_solo_safe(quest: dict) -> tuple[bool, str]:
    """Drop a quest when any objective is elite-only / has no reachable local
    source, or a giver/ender is missing. A 'provided' item objective is fine."""
    if not set(quest.get("behaviors", [])) <= SUPPORTED:
        return False, "unsupported_behavior"
    if not quest.get("starters"):
        return False, "no_giver"
    if not quest.get("enders"):
        return False, "no_ender"
    ref = quest["starters"][0]
    for obj in quest.get("objectives", []):
        if objective_target(obj, ref) is None:
            return False, "elite_or_no_solo_source"
    return True, ""


def order_quests(quests: list[dict]) -> list[dict]:
    """Level-first, then quest chain: a quest never precedes its prerequisite.
    A stable sort by (min_level, quest_level, id) already respects chains in the
    common case (prereqs are lower level); a topological nudge fixes the rest."""
    base = sorted(quests, key=lambda q: (q["min_level"], q["quest_level"], q["quest"]))
    by_id = {q["quest"]: q for q in base}
    placed: list[dict] = []
    seen: set[int] = set()

    def emit(q: dict) -> None:
        if q["quest"] in seen:
            return
        prev = q.get("chain", {}).get("previous", 0)
        if prev and prev in by_id and prev not in seen:
            emit(by_id[prev])
        seen.add(q["quest"])
        placed.append(q)

    for q in base:
        emit(q)
    return placed


def giver_point(quest: dict) -> dict:
    s = quest["starters"][0]
    return {"entry": s["entry"], "x": round(s["x"], 1), "y": round(s["y"], 1), "z": round(s["z"], 1)}


def ender_point(quest: dict) -> dict:
    e = quest["enders"][0]
    return {"entry": e["entry"], "x": round(e["x"], 1), "y": round(e["y"], 1), "z": round(e["z"], 1)}


def make_segment(quest: dict, target: int) -> dict | None:
    behaviors = set(quest.get("behaviors", []))
    giver = giver_point(quest)
    ender = ender_point(quest)
    qid = quest["quest"]
    slug = re.sub(r"[^a-z0-9]+", "-", quest["title"].lower()).strip("-")[:32]
    # Gate on the quest's real QuestMinLevel (the level below which it cannot be
    # accepted at all), NOT its recommended quest_level. Gating at quest_level
    # deadlocks a fresh level-1 bot: every quest (even the level-1-doable Cutting
    # Teeth) would be withheld until the bot already outleveled it, but it can't
    # level without questing. Per-pull readiness + death budgets (ADR-050) carry
    # the under-level safety that over-gating used to provide.
    min_level = min(quest["min_level"], target)

    ref = quest["starters"][0]
    kill_entries: list[dict] = []
    go_entries: list[dict] = []
    for obj in quest.get("objectives", []):
        tgt = objective_target(obj, ref)
        if tgt is None:
            return None
        if tgt == "provided":
            continue  # item handed over at accept -- nothing to fight/collect
        kind, entry, x, y, z = tgt
        row = {"entry": entry, "x": round(x, 1), "y": round(y, 1), "z": round(z, 1)}
        (go_entries if kind == "go" else kill_entries).append(row)

    # Pure delivery (no fight/collect): one atomic accept+turnin segment so a
    # min_level defer keeps the pair together and a quest that auto-completes on
    # accept (or cannot complete) is handled without churn.
    if not kill_entries and not go_entries:
        return {"id": f"q{qid}-{slug}", "type": "quest_delivery", "quest": qid,
                "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"],
                "giver_z": giver["z"], "turnin": ender["entry"], "turnin_x": ender["x"],
                "turnin_y": ender["y"], "turnin_z": ender["z"], "min_level": min_level}

    # Gameobject collection (optionally mixed with kills): accept, drive the GO
    # step over the object cluster, then hand in.
    if go_entries:
        first = go_entries[0]
        seg = {"id": f"q{qid}-{slug}-go", "type": "quest_gameobject", "quest": qid,
               "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"],
               "giver_z": giver["z"], "go_entries": go_entries,
               "x": first["x"], "y": first["y"], "z": first["z"], "radius": 120.0,
               "turnin": ender["entry"], "turnin_x": ender["x"], "turnin_y": ender["y"],
               "turnin_z": ender["z"], "min_level": min_level}
        if kill_entries:
            seg["kill_entries"] = kill_entries
        return seg

    # Kill / collection: one bundled quest_grind (multi-objective -> kill_entries).
    first = kill_entries[0]
    return {"id": f"q{qid}-{slug}", "type": "quest_grind", "quest": qid,
            "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"], "giver_z": giver["z"],
            "kill_entry": first["entry"], "x": first["x"], "y": first["y"], "z": first["z"],
            "kill_entries": kill_entries,
            "turnin": ender["entry"], "turnin_x": ender["x"], "turnin_y": ender["y"], "turnin_z": ender["z"],
            "min_level": min_level}


def build_route(existing: dict, coverage_variant: dict, target: int) -> tuple[dict, dict]:
    quests = [q for q in coverage_variant["quests"]
              if q["reason"] in ELIGIBLE_REASONS and q["quest_level"] <= target + 1]
    kept, dropped = [], {}
    for q in quests:
        ok, why = quest_is_solo_safe(q)
        (kept.append(q) if ok else dropped.setdefault(why, []).append(q["quest"]))

    ordered = order_quests(kept)

    segments: list[dict] = []
    grind_spot = None
    grind_spot_score = 1e9
    grind_ideal = max(1, target - 3)  # a top-off mob a few levels under the exit
    for q in ordered:
        seg = make_segment(q, target)
        if seg is None:
            dropped.setdefault("segment_build_failed", []).append(q["quest"])
            continue
        # Pick the grind mob whose quest_level sits ~3 under the exit level:
        # high enough to give real XP for the final push, low enough that a
        # plateaued bot can actually kill it (the readiness engine refuses
        # far-over-level pulls, and far-under-level mobs give gray XP).
        ke = (seg.get("kill_entries") or [None])[0]
        if ke:
            score = abs(q["quest_level"] - grind_ideal)
            if score < grind_spot_score:
                grind_spot, grind_spot_score = ke, score
        segments.append(seg)

    # ALWAYS append a grind-to-target fallback (not conditional). In practice
    # only ~a third of a starter zone's quests complete unattended (the rest
    # need use-item/interact-GO behaviors, cross-zone travel, or are phased),
    # so quests alone plateau a bot well below the exit level. A bot that
    # completes its doable quests and permanently skips the undoable ones would
    # otherwise finish its route stuck at ~level 4-5; this fallback grinds it
    # the rest of the way to the exit level. A bot already at the target when it
    # reaches here skips it instantly (seg_grind_to_level returns at once), so
    # this never forces grinding on a bot the quests already carried -- it only
    # rescues the ones the quests could not. Reducing this reliance is a matter
    # of unlocking more quest behaviors, tracked in the handoff.
    if grind_spot is not None:
        segments.append({
            "id": f"grind-to-{target}", "type": "grind_to_level", "level": target,
            "entry": grind_spot["entry"], "x": grind_spot["x"], "y": grind_spot["y"],
            "z": grind_spot["z"], "max_minutes": 180})

    route = {k: existing[k] for k in existing if k != "segments"}
    route["comment"] = (f"quest-first generated route (generate_routes.py) -- "
                        f"{len([s for s in segments if s['type'] != 'grind_to_level'])} quest segments, "
                        f"target level {target}")
    route["segments"] = segments
    stats = {"quests_kept": len(kept), "segments": len(segments),
             "grind_segments": sum(s["type"] == "grind_to_level" for s in segments),
             "dropped": {k: len(v) for k, v in dropped.items()}}
    return route, stats


def main() -> int:
    here = Path(__file__).resolve().parent
    p = argparse.ArgumentParser()
    p.add_argument("--coverage-dir", type=Path,
                   default=here.parent.parent.parent / "docs/autonomous-player/generated/coverage")
    p.add_argument("--config", type=Path, default=here / "coverage_families.json")
    p.add_argument("--existing-routes", type=Path, default=here / "routes")
    p.add_argument("--output-dir", type=Path, default=here / "routes_generated")
    args = p.parse_args()

    config = json.loads(args.config.read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)
    summary = {}
    for family in config["families"]:
        cov = json.loads((args.coverage_dir / f"{family['id']}.json").read_text())
        cov_by_route = {v["route"]: v for v in cov["variants"]}
        for variant in family["variants"]:
            rname = variant["route"]
            existing = json.loads((args.existing_routes / rname).read_text())
            target = target_level(rname)
            route, stats = build_route(existing, cov_by_route[rname], target)
            (args.output_dir / rname).write_text(json.dumps(route, indent=2) + "\n")
            summary[rname] = stats
            print(f"{rname:42s} quests={stats['quests_kept']:3d} "
                  f"segs={stats['segments']:3d} grind={stats['grind_segments']} "
                  f"dropped={sum(stats['dropped'].values())}")
    (args.output_dir / "_generation_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
