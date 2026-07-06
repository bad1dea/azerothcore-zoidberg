#!/usr/bin/env python3
"""Extract road-surface cells from the WoW 3.3.5a client for safe_path.

Python port of Likon69's road-extractor (~/research/Extractor_Mangos_Custom/
road-extractor/RoadExtractor.cpp, reviewed 2026-07-05): every ADT terrain
tile carries an MTEX texture-name table and 16x16 MCNK chunks whose MCLY
layers reference those textures -- a chunk painted with a road/path/
cobblestone texture IS the road, straight from the client's ground truth.
HonorBuddy bakes these into its navmesh as AreaType.Road (cost 1.0 vs ground
1.66); our equivalent feeds safe_path.py, whose A* then prefers genuine road
cells instead of merely spawn-free ground.

Only tiles the committed routes actually touch (plus a 1-tile margin) are
read. The ADT tile-index/world-axis convention is notoriously easy to flip,
so it is resolved empirically: both plausible conventions are computed and
the one whose road cells land on known road landmarks (Goldshire crossroads,
Razor Hill) wins.

Usage (on a host with the client):
    python3 extract_roads.py --client ~/wow_wotlk_3.3.5a > roads.json
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct
import sys

from mpyq import MPQArchive

TILE = 533.33333
CHUNK = TILE / 16.0

MAPS = {0: "Azeroth", 1: "Kalimdor", 530: "Expansion01"}

ROAD_TEXTURE_PATTERNS = (
    "road", "cobblestone", "path_cobble", "path_stone",
    "dirtpath", "dirt_path", "bridgefloor", "bridge_stone",
)

# Known on-road world positions used to auto-resolve the tile/axis
# convention: the road demonstrably passes within ~50yd of each.
LANDMARKS = {0: (-9459.5, 42.1), 1: (317.8, -4734.0)}


def mpq_chain(client: str) -> list[MPQArchive]:
    """Highest-priority first, mirroring the client's patch order."""
    names = ["patch-3.MPQ", "patch-2.MPQ", "patch.MPQ", "lichking.MPQ",
             "expansion.MPQ", "common-2.MPQ", "common.MPQ"]
    paths = [os.path.join(client, "Data", n) for n in names]
    locale = sorted(glob.glob(os.path.join(client, "Data", "??[SBU]?")) +
                    glob.glob(os.path.join(client, "Data", "en??")))
    for loc in locale:
        tag = os.path.basename(loc)
        for n in (f"patch-{tag}-3.MPQ", f"patch-{tag}-2.MPQ", f"patch-{tag}.MPQ"):
            p = os.path.join(loc, n)
            if os.path.exists(p):
                paths.insert(0, p)
    archives = []
    for p in paths:
        if os.path.exists(p):
            try:
                archives.append(MPQArchive(p, listfile=True))
            except Exception as exc:
                sys.stderr.write(f"skip {p}: {exc}\n")
    return archives


def read_file(archives: list[MPQArchive], path: str) -> bytes | None:
    for a in archives:
        try:
            data = a.read_file(path)
        except Exception:
            data = None
        if data:
            return data
    return None


def road_mask(data: bytes) -> list[int] | None:
    """Exact port of extractRoadMask: returns 256-entry 0/1 list (row*16+col)
    or None when the tile has no MTEX/road content."""
    size = len(data)
    if size < 20:
        return None
    mtex = None
    mcnks = []
    pos = 0
    while pos + 8 <= size:
        magic, csize = struct.unpack_from("<4sI", data, pos)
        if csize > size - pos - 8:
            break
        if magic == b"XETM":  # 'MTEX' little-endian on disk
            mtex = data[pos + 8:pos + 8 + csize]
        elif magic == b"KNCM":  # 'MCNK'
            mcnks.append(pos)
        pos += 8 + csize
    if not mtex or not mcnks:
        return None
    names = [n.decode("ascii", "replace").lower()
             for n in mtex.split(b"\0") if n]
    is_road = [any(p in n for p in ROAD_TEXTURE_PATTERNS) for n in names]
    if not any(is_road):
        return None
    mask = [0] * 256
    for ci, off in enumerate(mcnks[:256]):
        mcnk_size = struct.unpack_from("<I", data, off + 4)[0]
        end = off + 8 + mcnk_size
        if end > size:
            continue
        ofs_layer = struct.unpack_from("<I", data, off + 8 + 0x1C)[0]
        if not ofs_layer:
            continue
        mcly = off + ofs_layer
        if mcly + 8 > end:
            continue
        magic, lsize = struct.unpack_from("<4sI", data, mcly)
        if magic != b"YLCM":  # 'MCLY'
            continue
        for li in range(lsize // 16):
            tex_id = struct.unpack_from("<I", data, mcly + 8 + li * 16)[0]
            if tex_id < len(is_road) and is_road[tex_id]:
                mask[ci] = 1
                break
    return mask


def chunk_center(tx: int, ty: int, row: int, col: int,
                 swapped: bool) -> tuple[float, float]:
    """World center of MCNK (row, col) in ADT tile named Map_tx_ty.adt.
    Convention A (swapped=False): filename indices are (col-index, row-index)
    with worldX derived from ty. Convention B: the reverse. Resolved
    empirically against LANDMARKS."""
    a, b = (ty, tx) if not swapped else (tx, ty)
    x = (32 - a) * TILE - row * CHUNK - CHUNK / 2.0
    y = (32 - b) * TILE - col * CHUNK - CHUNK / 2.0
    return x, y


def route_tiles(tools_dir: str, map_for_family) -> dict[int, set]:
    pts: dict[int, list] = {0: [], 1: [], 530: []}
    for f in glob.glob(os.path.join(tools_dir, "routes_generated", "*.json")):
        if f.endswith("_generation_summary.json"):
            continue
        fam = os.path.basename(f).split("_")[0]
        m = map_for_family.get(fam)
        if m is None:
            continue
        d = json.load(open(f))
        for s in d["segments"]:
            for kx, ky in (("x", "y"), ("giver_x", "giver_y"),
                           ("turnin_x", "turnin_y")):
                if kx in s:
                    pts[m].append((s[kx], s[ky]))
            for row in s.get("points") or []:
                pts[m].append((row[0], row[1]))
            for row in (s.get("kill_entries") or []) + (s.get("clusters") or []):
                pts[m].append((row["x"], row["y"]))
    tiles: dict[int, set] = {}
    for m, plist in pts.items():
        t = set()
        for x, y in plist:
            base_a = int(32 - x / TILE)
            base_b = int(32 - y / TILE)
            for da in (-1, 0, 1):
                for db in (-1, 0, 1):
                    t.add((base_a + da, base_b + db))
        tiles[m] = t
    return tiles


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--client", required=True)
    ap.add_argument("--tools-dir",
                    default=os.path.dirname(os.path.abspath(__file__)))
    args = ap.parse_args()

    fam_map = {"durotar": 1, "mulgore": 1, "tirisfal": 0, "elwynn": 0,
               "dunmorogh": 0, "eversong": 530}
    archives = mpq_chain(os.path.expanduser(args.client))
    if not archives:
        sys.stderr.write("no MPQ archives found\n")
        return 1
    tiles = route_tiles(args.tools_dir, fam_map)

    # Read masks once per tile (keyed by raw filename indices), then decide
    # the coordinate convention against the landmarks.
    masks: dict[int, dict[tuple, list]] = {}
    for m, tset in tiles.items():
        name = MAPS[m]
        masks[m] = {}
        # The tile set was computed from world coords under BOTH possible
        # index orders (a/b symmetric +-1 margin), so try each pair as the
        # literal filename indices.
        candidates = set()
        for a, b in tset:
            candidates.add((a, b))
            candidates.add((b, a))
        for tx, ty in sorted(candidates):
            if not (0 <= tx < 64 and 0 <= ty < 64):
                continue
            data = read_file(archives,
                             f"World\\Maps\\{name}\\{name}_{tx}_{ty}.adt")
            if not data:
                continue
            mask = road_mask(data)
            if mask and any(mask):
                masks[m][(tx, ty)] = mask
        sys.stderr.write(f"map {m}: {len(masks[m])} tiles with road cells\n")

    def cells(swapped: bool) -> dict[int, list]:
        out = {}
        for m, tmap in masks.items():
            pts = []
            for (tx, ty), mask in tmap.items():
                for ci, flag in enumerate(mask):
                    if flag:
                        x, y = chunk_center(tx, ty, ci // 16, ci % 16, swapped)
                        pts.append((round(x, 1), round(y, 1)))
            out[m] = pts
        return out

    def landmark_error(c: dict[int, list]) -> float:
        err = 0.0
        for m, (lx, ly) in LANDMARKS.items():
            pts = c.get(m) or []
            if not pts:
                return 1e9
            err += min((px - lx) ** 2 + (py - ly) ** 2 for px, py in pts) ** 0.5
        return err

    ca, cb = cells(False), cells(True)
    ea, eb = landmark_error(ca), landmark_error(cb)
    chosen, err, tag = (ca, ea, "A") if ea <= eb else (cb, eb, "B")
    sys.stderr.write(
        f"convention {tag} chosen (landmark err {err:.0f}yd vs {max(ea, eb):.0f}yd)\n")
    if err > 120:
        sys.stderr.write("landmark error too large -- refusing to emit "
                         "(convention unresolved)\n")
        return 1
    json.dump({str(m): sorted(v) for m, v in chosen.items()}, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
