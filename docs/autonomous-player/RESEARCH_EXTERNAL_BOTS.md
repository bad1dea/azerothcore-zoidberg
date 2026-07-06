# External bot research (prior art)

Reviewed 2026-07-05 at the user's direction. All source is saved locally in
`/home/khuong/research/` — consult it BEFORE designing new navigation,
combat, or questing behavior. This file records what each system actually
does and what we adopted (or deliberately did not).

## CopilotBuddy / HonorBuddy lineage (Likon69, 13 repos)

`~/research/CopilotBuddy` (+ `Extractor_Mangos_Custom`, `extractor-csharp`,
`Navigation-C-`, `Quest-Behaviors`, `Questing-profiles`, `Singular-wotlk`,
`MeshViewer3D`, docs, and others). WotLK 3.3.5a client-side bot, API ported
from HonorBuddy; injected C#, Recast/Detour navigation.

**Travel = three mechanisms, none of them in the questing profiles:**

1. **Road preference is baked into the navmesh.**
   `road-extractor` reads ADT `MTEX` texture names from the client MPQs,
   pattern-matches road textures (`road`, `cobblestone`, `path_stone`,
   `dirtpath`, `bridgefloor`, …) and emits per-MCNK road masks; the retuned
   movemap generator tags those polys `AreaType.Road`. Query filter costs
   **Road=1.0, Ground=1.66** (`Tripper/Navigation/Navigator.cs`). Shortest
   path therefore follows roads whenever a detour costs < ~66% extra.
2. **Blackspots** (`Styx/Logic/Profiles/Blackspot.cs`): profile-authored
   no-go cylinders (X/Y/Z/radius/height) excluded by the navigator.
3. **AvoidanceManager** (`Styx/Logic/Pathing/AvoidanceManager.cs`): dynamic
   avoidance — tracks LIVE mobs (per-entry opt-in) within 60yd and lays
   temporary blackspots at their current positions (aggro radius + 10yd
   padding, updated on 10yd movement).

**Adopted into mod-autonomous-player (commits `722d728`, `133ab06`):**
- `tools/extract_roads.py` — Python port of road-extractor over the local
  client (`~/wow_wotlk_3.3.5a` on amy); tile/axis convention auto-resolved
  against road landmarks (28yd err vs 4292yd flipped). 3502 road cells,
  maps 0/1 (`tools/roads.json`). **Gap: map 530 Eversong matched zero road
  textures** — belf zones use other texture names; extend
  `ROAD_TEXTURE_PATTERNS` after inspecting Eversong MTEX names.
- `tools/safe_path.py` — grid A*: threat field from DB spawn snapshot
  (level-aware aggro radii) + live threats + blackspot cylinders + road
  cells at cost ×0.6 (HB's ratio). Roads attract, camps repel.
- `.autonomousplayer threats <char> <radius>` — live hostiles with REAL
  engine faction hostility; runner merges into the planner per long walk
  (our AvoidanceManager, adapted to the external-runner architecture).
- Routes may declare `"blackspots": [[x, y, r], ...]`.

**Not adopted (recorded for later):**
- Navmesh-level road tagging in AC's own mmaps (NAV_AREA_ROAD + PathGenerator
  area costs) — the extractor blueprint is complete in
  `Extractor_Mangos_Custom`; big slice, only worth it if the runner-side
  planner proves insufficient.
- `Singular-wotlk` combat routine — per-class rotation prior art; compare
  against `Combat::CastRotationAbility` when class kits feel weak.
- Their per-objective Hotspot circuits (8-point farm loops) — could enrich
  quest_grind camp rotation; farming-efficiency, not deaths.
- `CopilotBuddy/datadb/CreatureSpawns.db` — their spawn DB; cross-check
  against our `threat_spawns.json` if a zone's threat field looks wrong.

## BloogBot / Drew Kestell series (25 chapters)

`~/research/BloogBot` + https://drewkestell.us/Article/6/Chapter/1..25.
Vanilla/TBC/WotLK client-side bot, DLL injection, per-class bots.
Educational codebase — cleaner to read than CopilotBuddy, less complete.

- **Navigation (ch. 20):** same Recast/Detour approach (mmaps from the
  MaNGOS extractors), a thin C++ `Navigation` wrapper exposing
  `CalculatePath(mapId, start, end, smooth)`. No road preference, no mob
  avoidance — recalculates the whole path every update tick (25ms) which
  implicitly tracks moving targets. Lesson we already live by: per-hop
  re-issue + tight update beats following a stale path.
- **Autonomy (ch. 14):** stack-based state machine (GrindState ->
  MoveToTarget -> Combat, push/pop). Decentralized: each state manages its
  own transitions. Equivalent altitude to our GuideRuntime step/phase
  machine; nothing to port, but a good sanity reference for state-explosion
  discussions.
- **Hotspots (ch. 24):** grind areas = waypoint sets in a shared DB; bot
  roams RANDOM waypoints within the hotspot until targets appear.
  Random-waypoint roaming inside a camp is their anti-pattern-detection
  trick; our camp `points` rotation is the same idea, deterministic.
- Targeting priority (ch. 24): (1) whatever attacks you, (2) closest
  eligible hostile, (3) roam. Matches our ambient-defense-first ordering.

## Cross-cutting conclusions

- Nobody in this lineage had threat-aware transit planning; they relied on
  navmesh + human-authored pacing. Our spawn-field A* is genuinely beyond
  the prior art; the prior art's road tagging and live avoidance are what
  we adopted to complete it.
- All three systems treat "attacked while doing something else" as the
  highest-priority interrupt — validation for ambient self-defense.
- Authored data (profiles, hotspots, blackspots) always overrides derived
  data when they disagree — the same principle as our authored-routes
  invariant.
