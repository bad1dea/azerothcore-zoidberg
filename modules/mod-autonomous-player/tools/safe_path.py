#!/usr/bin/env python3
"""Threat-aware transit planner for route_runner long walks.

Long walks previously went point-to-point on the navmesh's shortest path,
which cuts straight through mob camps -- the dominant fleet death cause once
in-camp fixes landed (live: Humantwelve's corpses all along the Goldshire ->
wolf-camp line; Grunttwelve dying mid-vendor-run in the Razormane belt).

Model: hostile spawn points (threat_spawns.json, exported from acore_world by
export_threat_snapshot.py) radiate a level-aware threat cost; an A* over a
coarse 2D grid then minimizes distance x (1 + threat). Roads and other
spawn-free corridors are naturally the cheapest lanes, so the planner "sticks
to roads" without needing any road data. The runner walks the resulting hops
with its normal walk_toward machinery (navmesh per hop, divergence leash,
unstick), and falls back to the direct walk whenever planning fails --
this can only remove risk, never add a new failure mode.

Z per hop is approximated from the nearest exported spawn (spawns sit on the
ground); the engine's MoveTo z-snaps per hop, and hop lengths (~40yd) keep
the error inside its tolerance.
"""
from __future__ import annotations

import heapq
import json
import math
import os

CELL = 10.0            # grid cell size, yards
MARGIN = 200.0         # bbox margin around start/dest, yards
MAX_CELLS = 250_000    # refuse absurd grids (cross-continent requests)
HOP_SPACING = 75.0     # collapse the cell path to hops about this far apart

# HonorBuddy's navmesh costs roads 1.0 vs ordinary ground 1.66 (reviewed in
# ~/research/CopilotBuddy, Tripper Navigator). Same ratio here: a genuine
# road cell (extract_roads.py -- client ADT road textures, the ground truth
# HB's mesher used) is ~40% cheaper than open ground, so the A* follows
# real roads even through zero-threat terrain.
ROAD_FACTOR = 0.6
ROAD_REACH = 25.0      # a grid cell within this of a road center counts as road

# A blackspot (route-authored no-go cylinder, HB Blackspot equivalent) is
# heavily penalized rather than hard-blocked: if the ONLY way through is a
# blackspot, a long bad walk still beats no walk.
BLACKSPOT_COST = 200.0

_spawns_cache: dict[str, list] | None = None
_roads_cache: dict[str, list] | None = None


def _load(name: str) -> dict[str, list]:
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    try:
        with open(path) as fh:
            return json.load(fh)
    except Exception:
        return {}


def _spawns() -> dict[str, list]:
    global _spawns_cache
    if _spawns_cache is None:
        _spawns_cache = _load("threat_spawns.json")
    return _spawns_cache


def _roads() -> dict[str, list]:
    global _roads_cache
    if _roads_cache is None:
        _roads_cache = _load("roads.json")
    return _roads_cache


def _threat_radius(mob_level: int, bot_level: int) -> float:
    """Approximate aggro + chain-assist reach. Zero for mobs the bot
    outlevels by 5+ (gray, no aggro); grows with the level deficit."""
    delta = mob_level - bot_level
    if delta <= -5:
        return 0.0
    return min(40.0, 14.0 + 3.0 * (delta + 4))


def plan(map_id: int | str, sx: float, sy: float, dx: float, dy: float,
         bot_level: int,
         extra_threats: list | None = None,
         blackspots: list | None = None) -> list[tuple[float, float, float]] | None:
    """Hop list from (sx,sy) to (dx,dy) avoiding threat, or None when
    planning is not possible/meaningful (missing data, oversized grid,
    no route). The destination itself is always the last hop -- reaching
    a deliberately hot destination (a quest camp) stays the caller's
    business; this only shapes the approach.

    extra_threats: live mob positions [[x, y, level], ...] from the
    `.autonomousplayer threats` command -- the HB AvoidanceManager idea:
    static spawn anchors miss wandering patrols, live positions don't.
    blackspots: route-authored no-go cylinders [[x, y, radius], ...]."""
    spawns = _spawns().get(str(map_id)) or []

    x_lo, x_hi = min(sx, dx) - MARGIN, max(sx, dx) + MARGIN
    y_lo, y_hi = min(sy, dy) - MARGIN, max(sy, dy) + MARGIN
    nx = int((x_hi - x_lo) / CELL) + 1
    ny = int((y_hi - y_lo) / CELL) + 1
    if nx * ny > MAX_CELLS:
        return None

    # Threat field: additive per spawn so packs cost more than stragglers.
    threat = [0.0] * (nx * ny)
    local: list[list[float]] = []
    for x, y, z, lvl in spawns:
        if x_lo - 45 <= x <= x_hi + 45 and y_lo - 45 <= y <= y_hi + 45:
            r = _threat_radius(int(lvl), bot_level)
            if r > 0:
                local.append([x, y, z, r])
    for x, y, lvl in (extra_threats or []):
        if x_lo - 45 <= x <= x_hi + 45 and y_lo - 45 <= y <= y_hi + 45:
            # +6yd padding over the static radius: a live mob is a fact,
            # not a spawn-point guess (HB pads live avoidance the same way).
            r = _threat_radius(int(lvl), bot_level)
            if r > 0:
                local.append([x, y, 0.0, r + 6.0])

    # Road bonus lanes (client ADT ground truth; see ROAD_FACTOR above).
    road = [False] * (nx * ny)
    road_pts = [(x, y) for x, y in (_roads().get(str(map_id)) or [])
                if x_lo - ROAD_REACH <= x <= x_hi + ROAD_REACH
                and y_lo - ROAD_REACH <= y <= y_hi + ROAD_REACH]
    for x, y in road_pts:
        cr = int(ROAD_REACH / CELL) + 1
        cx0, cy0 = int((x - x_lo) / CELL), int((y - y_lo) / CELL)
        for gx in range(max(0, cx0 - cr), min(nx, cx0 + cr + 1)):
            for gy in range(max(0, cy0 - cr), min(ny, cy0 + cr + 1)):
                if math.hypot(x_lo + gx * CELL - x, y_lo + gy * CELL - y) <= ROAD_REACH:
                    road[gy * nx + gx] = True

    if not local and not road_pts and not blackspots:
        return None  # nothing to avoid or prefer; direct walk is optimal
    # Cheap-leg short-circuit: hop-following costs ~20s per hop (per-hop
    # navmesh walk + SOAP polling), which turned every level-1 starter
    # commute into a 4-6 minute weave. If the DIRECT line is already
    # cold, walk it.
    if not blackspots and path_threat_from(local, [(sx, sy), (dx, dy)]) < 4.0:
        return None
    for x, y, _z, r in local:
        cr = int(r / CELL) + 1
        cx0, cy0 = int((x - x_lo) / CELL), int((y - y_lo) / CELL)
        for gx in range(max(0, cx0 - cr), min(nx, cx0 + cr + 1)):
            for gy in range(max(0, cy0 - cr), min(ny, cy0 + cr + 1)):
                px, py = x_lo + gx * CELL, y_lo + gy * CELL
                d = math.hypot(px - x, py - y)
                if d < r:
                    threat[gy * nx + gx] += 12.0 * (1.0 - d / r)
    for x, y, r in (blackspots or []):
        cr = int(r / CELL) + 1
        cx0, cy0 = int((x - x_lo) / CELL), int((y - y_lo) / CELL)
        for gx in range(max(0, cx0 - cr), min(nx, cx0 + cr + 1)):
            for gy in range(max(0, cy0 - cr), min(ny, cy0 + cr + 1)):
                if math.hypot(x_lo + gx * CELL - x, y_lo + gy * CELL - y) < r:
                    threat[gy * nx + gx] += BLACKSPOT_COST

    start = (int((sx - x_lo) / CELL), int((sy - y_lo) / CELL))
    goal = (int((dx - x_lo) / CELL), int((dy - y_lo) / CELL))

    def h(c):  # admissible: straight-line at the cheapest possible rate
        return ROAD_FACTOR * math.hypot(c[0] - goal[0], c[1] - goal[1])

    dist = {start: 0.0}
    prev: dict[tuple[int, int], tuple[int, int]] = {}
    pq = [(h(start), start)]
    steps = [(-1, -1, 1.4142), (-1, 0, 1.0), (-1, 1, 1.4142), (0, -1, 1.0),
             (0, 1, 1.0), (1, -1, 1.4142), (1, 0, 1.0), (1, 1, 1.4142)]
    found = False
    while pq:
        _, cur = heapq.heappop(pq)
        if cur == goal:
            found = True
            break
        base = dist[cur]
        for ddx, ddy, w in steps:
            nxt = (cur[0] + ddx, cur[1] + ddy)
            if not (0 <= nxt[0] < nx and 0 <= nxt[1] < ny):
                continue
            idx = nxt[1] * nx + nxt[0]
            step_cost = w * (1.0 + threat[idx])
            if road[idx]:
                step_cost *= ROAD_FACTOR
            cost = base + step_cost
            if cost < dist.get(nxt, float("inf")):
                dist[nxt] = cost
                prev[nxt] = cur
                heapq.heappush(pq, (cost + h(nxt), nxt))
    if not found:
        return None

    cells = [goal]
    while cells[-1] != start:
        cells.append(prev[cells[-1]])
    cells.reverse()

    # Collapse to ~HOP_SPACING hops; z from the nearest exported spawn.
    def z_at(px: float, py: float, fallback: float) -> float:
        best, bz = 1e18, fallback
        for x, y, z, _r in local:
            d2 = (x - px) ** 2 + (y - py) ** 2
            if d2 < best:
                best, bz = d2, z
        return bz if best <= 80.0 ** 2 else fallback

    hops: list[tuple[float, float, float]] = []
    acc = 0.0
    for i in range(1, len(cells)):
        acc += CELL * (1.4142 if cells[i][0] != cells[i - 1][0]
                       and cells[i][1] != cells[i - 1][1] else 1.0)
        if acc >= HOP_SPACING:
            acc = 0.0
            px = x_lo + cells[i][0] * CELL
            py = y_lo + cells[i][1] * CELL
            hops.append((px, py, z_at(px, py, 0.0)))
    return hops  # caller appends the true destination itself


def path_threat_from(local: list, points: list) -> float:
    """Total threat along a polyline against an already-built local spawn
    list [[x, y, z, radius], ...] (the plan()-internal representation)."""
    total = 0.0
    for i in range(1, len(points)):
        ax, ay = points[i - 1]
        bx, by = points[i]
        seg = math.hypot(bx - ax, by - ay)
        n = max(2, int(seg / 10.0))
        for k in range(n + 1):
            px, py = ax + (bx - ax) * k / n, ay + (by - ay) * k / n
            for x, y, _z, r in local:
                if r > 0 and (x - px) ** 2 + (y - py) ** 2 < r * r:
                    total += 1.0
    return total


def path_threat(map_id: int | str, points: list[tuple[float, float]],
                bot_level: int) -> float:
    """Total threat sampled along a polyline -- used by tests and for
    comparing planned vs direct routes."""
    spawns = _spawns().get(str(map_id)) or []
    total = 0.0
    for i in range(1, len(points)):
        ax, ay = points[i - 1]
        bx, by = points[i]
        seg = math.hypot(bx - ax, by - ay)
        n = max(2, int(seg / 10.0))
        for k in range(n + 1):
            px, py = ax + (bx - ax) * k / n, ay + (by - ay) * k / n
            for x, y, _z, lvl in spawns:
                r = _threat_radius(int(lvl), bot_level)
                if r > 0 and (x - px) ** 2 + (y - py) ** 2 < r * r:
                    total += 1.0
    return total
