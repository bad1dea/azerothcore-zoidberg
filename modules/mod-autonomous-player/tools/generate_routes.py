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
  * grinding is a bounded top-off/backstop: the generator reuses the existing
    route's live-authored grind rungs, camps, and unstick anchors rather than
    inventing camps from arbitrary quest objective spawns.

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

# Quests whose creature objective is credited by USING a provided quest item
# on the target (guidestartuseitemunit), not by killing it. The coverage
# compiler classifies them as plain "kill" (RequiredNpcOrGo is a creature), but
# the target never dies and a kill grind can never credit -- live: q5441 Lazy
# Peons, Foreman's Blackjack 16114 wakes Sleeping Peons (spell 19938 only lands
# while the Peon Sleep aura is up; the guide step's target sweep handles that).
# Keyed explicitly: detecting these generically needs the start item's spell
# target data, which the coverage snapshot does not carry.
USEITEM_UNIT_QUESTS = {5441: 16114}

# Quests whose giver/objective sits in territory far above the quest's DB
# accept-level. _combat_min_level only gates on kill targets, so delivery
# and GO quests keep min_level ~1 even when the WALK is lethal -- live:
# q8 "A Rogue's Deal" starts at Agamand Mills among 7-9 Darkhounds (a
# level-6 Priest fed 3+ deaths to it); q16 "Give Gerard a Drink" starts
# at the Maclure farms at the far SW corner of Elwynn, and because its
# min_level 1 sorts it FIRST, a fresh level-1 marched the whole zone
# through the Fargodeep kobold hills to reach it (3 deaths in 10 min).
# Breadcrumb deliveries: accept-level 1 quests whose whole point is moving
# you OUT of the starter camp at ~5 (Blizzard's zone flow). Sorted at
# min_level 1 they march fresh level-1-2 bots across aggro belts -- live,
# three zones in one night: q2161 A Peon's Burden killed Korgath/Locktwelve/
# Trolltwelve in the Razormane belt, q16 marched Humantwelve across Elwynn,
# q1656 A Task Unfinished killed Earthmane/Stormhoof to Prairie Wolves on
# the Bloodhoof road. The full set below is the data sweep of every
# committed route: quest_delivery with min_level<5 and giver->turnin
# distance >400yd (level-4 class quests 1520/1521 and short low-threat
# Eversong 9119 deliberately excluded). q8/q16 keep their higher floors.
QUEST_MIN_LEVEL_FLOORS = {
    8: 9, 16: 6,
    54: 5, 61: 5, 282: 5, 310: 5, 311: 5, 318: 5, 320: 5, 383: 5,
    420: 5, 805: 5, 823: 5, 828: 5, 1656: 5, 2158: 5, 2160: 5,
    2161: 5, 8347: 5, 8350: 5, 24857: 5,
    # 787 The New Horde: recovered by the Zygor-pilot bounding-box widen
    # (giver Grull Hawkwind sits in Orgrimmar, turnin back in the Valley of
    # Trials -- a genuine ~2500yd round trip). min_level 1 sorted it right
    # after the very first quest; live, 2026-07-07: all 7 Durotar bots hit
    # DISPLACED (2500+yd from segment anchor) simultaneously at level 1,
    # same breadcrumb-delivery pattern this floor list already exists for.
    787: 5,
}

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
    """From candidate spawn points ``(kind, entry, x, y, z, maxlevel)``, pick the one in
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
        _, e, x, y, _, _ = p
        return sum(1 for (_, e2, x2, y2, _, _) in points
                   if e2 == e and (x2 - x) ** 2 + (y2 - y) ** 2 <= 30 * 30)

    def refd(p: tuple) -> float:
        _, _, x, y, _, _ = p
        return ((x - ref["x"]) ** 2 + (y - ref["y"]) ** 2) if ref else 0.0

    return max(points, key=lambda p: (density(p), -refd(p)))


def objective_target(obj: dict, ref: dict | None):
    """Normalize one objective (schemas differ by type) into the solo-safest,
    densest action target:

      * ('kill'|'go', entry, x, y, z, maxlevel) -- a non-elite creature / world
        object to use, chosen from the densest same-entry cluster;
      * "provided"  -- an item handed over at accept, no action needed;
      * None        -- elite-only, or no local source: quest not solo-doable.

    `kill` objectives carry a creature `entry` with flat-coordinate sources;
    `item` (collection) objectives carry an `item` whose sources are creatures
    or objects with nested `spawns`; `gameobject` objectives carry a GO `entry`
    with flat-coordinate sources."""
    t = obj.get("type")
    if t == "gameobject":
        pts = [("go", obj["entry"], s["x"], s["y"], s["z"], 0)
               for s in obj.get("local_sources", [])]
        return _densest(pts, ref)
    if t == "kill":
        pts = [("kill", obj["entry"], s["x"], s["y"], s["z"], int(s.get("maxlevel", 1)))
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
                    pts.append((kind, src["entry"], sp["x"], sp["y"], sp["z"],
                                int(sp.get("maxlevel", 0))))
        # Prefer a non-combat source when one exists. Otherwise choose among the
        # lowest-level creature family that can drop the item. The old policy
        # pooled every source and let raw spawn density win, which could select
        # a level-8 family for a level-5 quest even though a level-5 family
        # dropped the same item.
        go_pts = [p for p in pts if p[0] == "go"]
        if go_pts:
            return _densest(go_pts, ref)
        if pts:
            easiest = min(p[5] for p in pts)
            pts = [p for p in pts if p[5] == easiest]
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


def order_quests(quests: list[dict], guide_priority: dict[int, int] | None = None) -> list[dict]:
    """Level-first, then quest chain: a quest never precedes its prerequisite.
    A stable sort by (min_level, quest_level, id) already respects chains in the
    common case (prereqs are lower level); a topological nudge fixes the rest.

    `guide_priority` (optional, quest id -> a human guide's own step index, e.g.
    from a Zygor-derived research source): when given, quests it covers sort by
    that natural encounter order instead of pure level -- a curated guide's
    order already bundles a quest hub's accepts together and routes to the next
    hub, rather than the mechanical "level 4 quest, then level 4 quest,
    regardless of which side of the map it's on" churn a level-only sort
    produces. Quests the guide doesn't cover (e.g. deliberate additions the
    guide never mentioned) still fall back to (min_level, quest_level, id), and
    always sort after every guide-covered quest at the same or lower level so
    they don't get inserted mid-hub. The prerequisite recursion below is
    unaffected either way -- a quest's own prereq is always emitted first
    regardless of which key placed it."""
    guide_priority = guide_priority or {}

    def sort_key(q: dict):
        p = guide_priority.get(q["quest"])
        # (0, priority, ...) sorts before (1, ...) -- guide-covered quests
        # lead, ties broken by the guide's own order; everything else falls
        # back to level order after them.
        if p is not None:
            return (0, p, q["min_level"], q["quest_level"], q["quest"])
        return (1, 0, q["min_level"], q["quest_level"], q["quest"])

    base = sorted(quests, key=sort_key)
    by_id = {q["quest"]: q for q in base}
    placed: list[dict] = []
    seen: set[int] = set()

    def emit(q: dict) -> None:
        if q["quest"] in seen:
            return
        prev = abs(int(q.get("chain", {}).get("previous", 0)))
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


def _quest_nav(existing: dict, quest_id: int) -> dict:
    """Carry forward the hand-authored route's proven navigation metadata.

    Coverage generation is authoritative for quest/objective data, but the old
    profiles contain live-earned cave approaches, hub unsticks, and turn-in
    detours that cannot be reconstructed from spawn rows. Preserve those
    fields whenever the quest was already authored.
    """
    out: dict = {}
    for old in existing.get("segments", []):
        if old.get("quest") != quest_id:
            continue
        if old.get("unstick") and "unstick" not in out:
            out["unstick"] = old["unstick"]
        if old.get("turnin_unstick"):
            out["turnin_unstick"] = old["turnin_unstick"]
        if old.get("giver_via"):
            out["giver_via"] = old["giver_via"]
        if old.get("turnin_via"):
            out["turnin_via"] = old["turnin_via"]
        if old.get("type") == "quest_accept" and old.get("via"):
            out["giver_via"] = old["via"]
        if old.get("type") == "quest_turnin" and old.get("via"):
            out["turnin_via"] = old["via"]
    return out


def _nearest_unstick(existing: dict, x: float, y: float) -> str | None:
    candidates = []
    for old in existing.get("segments", []):
        anchor = old.get("unstick")
        ox = old.get("x", old.get("giver_x"))
        oy = old.get("y", old.get("giver_y"))
        if anchor and ox is not None and oy is not None:
            candidates.append(((ox - x) ** 2 + (oy - y) ** 2, anchor))
    # Stable distance-only minimum: when two authored segments share a point
    # but name different later hubs, preserve the earlier route segment's local
    # anchor instead of breaking the tie lexicographically (live q3902 was in
    # Deathknell but incorrectly inherited DKBrill this way).
    return min(candidates, key=lambda c: c[0])[1] if candidates else None


def _combat_min_level(quest: dict, targets: list[tuple], target: int) -> int:
    """Do not schedule normal combat more than one level below the strongest
    selected mob.

    Accept-level is a database eligibility gate, not a solo-safety signal.
    Live fleet evidence showed quests with QuestMinLevel 1 sending level-4/5
    bots against level-7/8 mobs (a 3-level deficit) -- that's what this gate
    exists to prevent. The original fix required exact parity (0-level
    deficit) for any mob above level 2, which is more conservative than the
    incident needed and, combined with a fuller Zygor-informed quest set,
    was observed forcing multi-level waits (e.g. a level-2 bot deferring
    everything until level 5) even though Zygor's own real-play pacing
    accepts this content well before then -- the DB's real QuestMinLevel for
    those quests is 1-3, not 3-5; the inflation was entirely this gate. A
    1-level deficit (generalizing the old level<=2 starter-mob exception to
    every mob level, not just the first one) is the requested relaxation:
    watch fleet death rate after this ships, tighten back to 0 if it bites.
    """
    max_mob = max((t[5] for t in targets if t != "provided" and t[0] == "kill"),
                  default=0)
    safe_mob_level = max(max_mob - 1, 1) if max_mob else 0
    return min(max(int(quest["min_level"]), safe_mob_level), target)


def make_segment(quest: dict, target: int, existing: dict) -> dict | None:
    giver = giver_point(quest)
    ender = ender_point(quest)
    qid = quest["quest"]
    slug = re.sub(r"[^a-z0-9]+", "-", quest["title"].lower()).strip("-")[:32]
    ref = quest["starters"][0]
    targets: list[tuple] = []
    kill_entries: list[dict] = []
    go_entries: list[dict] = []
    for obj in quest.get("objectives", []):
        tgt = objective_target(obj, ref)
        if tgt is None:
            return None
        if tgt == "provided":
            continue  # item handed over at accept -- nothing to fight/collect
        targets.append(tgt)
        kind, entry, x, y, z, mlevel = tgt
        row = {"entry": entry, "x": round(x, 1), "y": round(y, 1), "z": round(z, 1)}
        if kind != "go":
            # Carry the creature's real level so grind rungs can be built from
            # level-appropriate mobs instead of a mislabeled tier (the gray-
            # camp stall: a "tier-6" rung on a level-3 mob = zero XP).
            row["maxlevel"] = int(mlevel)
            # ...and the spawn count: a grind rung needs a POPULATION to farm,
            # not a single-spawn named mob. Live 2026-07-08: mob_for_tier
            # picked entry 8554 "Chief Sharptusk Thornmantle" (a level-5 NAMED
            # unique, 1 spawn, ringed by guards) as Mulgore's tier-5 camp;
            # Tanktwelve/Bloodhorn couldn't pull it (UnsafeEncounter, guards)
            # and the grind failed every cycle for over an hour.
            row["spawns"] = sum(1 for s in obj.get("local_sources", [])
                                if int(s.get("rank", 0)) == 0)
        (go_entries if kind == "go" else kill_entries).append(row)
    min_level = max(_combat_min_level(quest, targets, target),
                    QUEST_MIN_LEVEL_FLOORS.get(qid, 0))
    nav = _quest_nav(existing, qid)
    if "unstick" not in nav:
        action = (kill_entries or go_entries or [giver])[0]
        anchor = _nearest_unstick(existing, action["x"], action["y"])
        if anchor:
            nav["unstick"] = anchor

    prev = abs(int(quest.get("chain", {}).get("previous", 0)))
    gate = {"requires_quest": prev} if prev else {}

    # Pure delivery (no fight/collect): one atomic accept+turnin segment so a
    # min_level defer keeps the pair together and a quest that auto-completes on
    # accept (or cannot complete) is handled without churn.
    if not kill_entries and not go_entries:
        return {"id": f"q{qid}-{slug}", "type": "quest_delivery", "quest": qid,
                "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"],
                "giver_z": giver["z"], "turnin": ender["entry"], "turnin_x": ender["x"],
                "turnin_y": ender["y"], "turnin_z": ender["z"],
                "min_level": min_level,
                **gate, **nav}

    # Gameobject collection (optionally mixed with kills): accept, drive the GO
    # step over the object cluster, then hand in.
    if go_entries:
        first = go_entries[0]
        seg = {"id": f"q{qid}-{slug}-go", "type": "quest_gameobject", "quest": qid,
               "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"],
               "giver_z": giver["z"], "go_entries": go_entries,
               "x": first["x"], "y": first["y"], "z": first["z"], "radius": 120.0,
               "turnin": ender["entry"], "turnin_x": ender["x"], "turnin_y": ender["y"],
               "turnin_z": ender["z"], "min_level": min_level, **gate, **nav}
        if kill_entries:
            seg["kill_entries"] = kill_entries
        return seg

    # Item-use-on-creature: the "kill" objective is really "use the provided
    # quest item on the creature" -- emit the dedicated segment instead of a
    # grind that can never credit.
    use_item = USEITEM_UNIT_QUESTS.get(qid)
    if use_item:
        first = kill_entries[0]
        return {"id": f"q{qid}-{slug}-useitem", "type": "quest_useitem_unit",
                "quest": qid, "item": use_item, "npc_entry": first["entry"],
                "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"],
                "giver_z": giver["z"], "clusters": kill_entries,
                "x": first["x"], "y": first["y"], "z": first["z"], "radius": 120.0,
                "turnin": ender["entry"], "turnin_x": ender["x"], "turnin_y": ender["y"],
                "turnin_z": ender["z"], "min_level": int(quest["min_level"]),
                **gate, **nav}

    # Kill / collection: one bundled quest_grind (multi-objective -> kill_entries).
    first = kill_entries[0]
    return {"id": f"q{qid}-{slug}", "type": "quest_grind", "quest": qid,
            "giver": giver["entry"], "giver_x": giver["x"], "giver_y": giver["y"], "giver_z": giver["z"],
            "kill_entry": first["entry"], "x": first["x"], "y": first["y"], "z": first["z"],
            "kill_entries": kill_entries,
            "turnin": ender["entry"], "turnin_x": ender["x"], "turnin_y": ender["y"], "turnin_z": ender["z"],
            "min_level": min_level, **gate, **nav}


def build_route(existing: dict, coverage_variant: dict, target: int,
                 guide_priority: dict[int, int] | None = None) -> tuple[dict, dict]:
    quests = [q for q in coverage_variant["quests"]
              if q["reason"] in ELIGIBLE_REASONS and q["quest_level"] <= target + 1]
    kept, dropped = [], {}
    for q in quests:
        ok, why = quest_is_solo_safe(q)
        (kept.append(q) if ok else dropped.setdefault(why, []).append(q["quest"]))

    # A generated profile starts from a fresh character. If a quest's explicit
    # prerequisite is absent/unsupported, the quest is not independently
    # runnable; attempting it only produces a failed accept followed by a
    # pointless objective walk. Remove blocked descendants transitively.
    while True:
        kept_ids = {q["quest"] for q in kept}
        blocked = [q for q in kept
                   if abs(int(q.get("chain", {}).get("previous", 0)))
                   and abs(int(q["chain"]["previous"])) not in kept_ids]
        if not blocked:
            break
        for q in blocked:
            kept.remove(q)
            dropped.setdefault("missing_prerequisite", []).append(q["quest"])

    ordered = order_quests(kept, guide_priority)

    # Build quest segments tagged with a safe level key plus the hand-authored
    # grind ladder, interleaved by level. Interleaving is essential: a plateaued
    # bot must reach a safe rung before higher objectives rather than traverse
    # every unsupported quest first. A bot already past a rung skips it.
    guide_priority = guide_priority or {}
    entries: list[tuple] = []  # (level_key, order_tiebreak, seg)
    kill_mobs: list[tuple] = []  # (quest_level, kill_entry) for kept kill quests
    quest_segs: list[dict] = []  # in guide/emit order, for hub grouping below
    for i, q in enumerate(ordered):
        seg = make_segment(q, target, existing)
        if seg is None:
            dropped.setdefault("segment_build_failed", []).append(q["quest"])
            continue
        # Stamp the guide's own step index onto the segment. The runtime has
        # no other way to know a quest is guide-covered or where it sits in
        # the guide's order: guide_priority is consumed here at generation
        # and never travels to route_runner.py otherwise. This is what lets
        # the runner log "a guide quest was available" honestly (grind is
        # only a real fallback if NO guide quest could run) and lets the
        # metrics report detect when a bot works quests out of guide order.
        gstep = guide_priority.get(q["quest"])
        if gstep is not None:
            seg["guide_step"] = gstep
        ke = (seg.get("kill_entries") or [None])[0]
        if ke:
            kill_mobs.append((q["quest_level"], ke))
        entries.append((max(min(q["quest_level"], target), seg.get("min_level", 1)), i, seg))
        quest_segs.append(seg)

    # Group contiguous guide steps into hub blocks so the runtime and the
    # metrics report can talk about "which hub" a bot is working, not just
    # which quest. A hub is a run of guide steps with no large gap between
    # them -- a curated guide keeps one hub's quests adjacent in its step
    # numbering, then jumps when it walks to the next hub. Quests with no
    # guide step (deliberate DB-discovered additions the guide never named)
    # attach to the nearest preceding hub so they don't each become a
    # singleton. Purely a metadata label; it does not reorder anything.
    HUB_STEP_GAP = 4
    stepped = sorted((s for s in quest_segs if "guide_step" in s),
                     key=lambda s: s["guide_step"])
    hub = 0
    prev_step = None
    for s in stepped:
        if prev_step is not None and s["guide_step"] - prev_step > HUB_STEP_GAP:
            hub += 1
        s["hub"] = hub
        prev_step = s["guide_step"]
    if stepped:
        # Non-guide quests inherit the hub of the nearest guide quest at or
        # below their sort position (fall back to hub 0).
        last_hub = 0
        for s in quest_segs:
            if "hub" in s:
                last_hub = s["hub"]
            else:
                s["hub"] = last_hub

    # Grind rungs are built from LEVEL-APPROPRIATE mobs, chosen by each mob's
    # REAL creature level -- not the quest level, and not a hand-authored
    # camp's (frequently mislabeled) mob. The gray-camp stall (2026-07-08,
    # ~half the fleet un-dinged 8-12h): "tier-6" rungs sat on level-3 Kobold
    # Workers and level-1 Volatile Mutations, giving a level-8 bot zero XP,
    # and when the bot's real tiers were hazard-condemned it fell back onto
    # those gray camps forever, unable to level enough to expire the
    # condemnation. Every rung must camp a mob near its own tier.
    grind_pool = [ke for s in quest_segs
                  for ke in (s.get("kill_entries") or [])
                  if ke.get("maxlevel", 0) > 0]
    GRINDABLE = 4  # a real camp has a population; fewer spawns = named/rare

    def mob_for_tier(tier: int):
        # Prefer a mob at or just below the tier (green/yellow: real XP AND
        # survivable at gear floor). Heavily penalize gray (>3 levels under
        # -> ~0 XP), too-hard (over the tier -> orange/red), and -- critically
        # -- SPARSE spawns (a single-spawn named/rare mob can't be farmed;
        # pulling it aggros its guards). No in-range mob -> closest available.
        if not grind_pool:
            return None

        # Target GREEN, not white: a mob ~1-2 levels BELOW the tier gives
        # strong XP and is survivable at the gear floor these bots grind on.
        # Camping AT-level (white/yellow) mobs got the mid-level fleet killed
        # (2026-07-08: Baldrick/Humantwelve death-looping their grind camps,
        # 24 deaths/15m). Above-tier (orange/red) is worst; gray (>3 under)
        # is no-XP. So: prefer [tier-2..tier-1], accept down to tier-3, avoid
        # at/above tier, avoid gray, and never a sparse/named mob.
        ideal = tier - 1
        def score(m):
            ml = m["maxlevel"]
            if m.get("spawns", 1) < GRINDABLE:
                return 4000 + (GRINDABLE - m.get("spawns", 1))  # not farmable
            if ml > tier:
                return 600 + (ml - tier) * 20   # white/orange: deadly, avoid
            if ml < tier - 3:
                return 1000 + (tier - ml)       # gray: no XP
            return abs(ml - ideal)              # green band, prefer tier-1
        return min(grind_pool, key=score)

    # Carry the family's proven zone hubs (unstick anchor, repair/vendor) from
    # any hand-authored grind rung -- those are zone location hints independent
    # of which mob we camp; only the mob and its coords are being corrected.
    authored = [s for s in existing.get("segments", [])
                if s.get("type") == "grind_to_level"]
    zone_unstick = next((s.get("unstick") for s in authored if s.get("unstick")), None)
    zone_vendor = (next((s.get("vendor") for s in authored if s.get("vendor")), None)
                   or existing.get("home_vendor"))

    # Authored families keep their authored TIER LEVELS (the ladder shape that
    # was tuned for the zone); families with no baseline get a 3..target
    # ladder. Either way the MOB at each tier is now level-correct.
    tiers = (sorted({int(s["level"]) for s in authored if int(s.get("level", 0)) <= target})
             or sorted(set(list(range(3, target, 3)) + [target])))
    for tier in tiers:
        mob = mob_for_tier(tier)
        if mob is None:
            break
        rung = {"id": f"grind-to-{tier}", "type": "grind_to_level", "level": tier,
                "entry": mob["entry"], "x": mob["x"], "y": mob["y"], "z": mob["z"],
                "mob_level": mob["maxlevel"], "max_minutes": 120}
        if zone_unstick:
            rung["unstick"] = zone_unstick
        if zone_vendor:
            rung["vendor"] = zone_vendor
        entries.append((tier, 10_000 + tier, rung))

    entries.sort(key=lambda e: (e[0], e[1]))
    segments = [e[2] for e in entries]

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
    p.add_argument("--guide-priority-dir", type=Path, default=here / "guide_priority",
                    help="Optional per-route quest-id -> step-index files (e.g. from a Zygor "
                         "Guides extraction) used to order same-level quests the way a human "
                         "guide bundles them by hub, instead of arbitrary same-level order. "
                         "Missing file for a route == no change from level-only ordering.")
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
            priority_file = args.guide_priority_dir / rname
            guide_priority = (
                {int(k): v for k, v in json.loads(priority_file.read_text()).items()}
                if priority_file.exists() else None
            )
            route, stats = build_route(existing, cov_by_route[rname], target, guide_priority)
            (args.output_dir / rname).write_text(json.dumps(route, indent=2) + "\n")
            summary[rname] = stats
            print(f"{rname:42s} quests={stats['quests_kept']:3d} "
                  f"segs={stats['segments']:3d} grind={stats['grind_segments']} "
                  f"dropped={sum(stats['dropped'].values())}")
    (args.output_dir / "_generation_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
