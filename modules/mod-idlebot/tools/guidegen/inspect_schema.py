#!/usr/bin/env python3
"""Inspect AzerothCore acore_world DB schema for guide generation.

Dumps column names/types for all tables relevant to quest/NPC/GO data.
Saves to data/generated/schema.json.
"""

import subprocess, json, sys, os

DB_HOST = "10.10.30.20"
DB_PASS = "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53"
OUT_DIR = os.path.join(os.path.dirname(__file__), "../../data/generated")

TABLES = [
    "quest_template", "quest_template_addon",
    "creature_queststarter", "creature_questender",
    "gameobject_queststarter", "gameobject_questender",
    "creature", "creature_template",
    "gameobject", "gameobject_template",
    "item_template",
    "playercreateinfo",
    "quest_poi", "quest_poi_points",
]


def q(sql):
    r = subprocess.run(
        ["ssh", f"khuong@{DB_HOST}",
         f"docker exec ac-database mysql -u root -p{DB_PASS} -N -e \"{sql}\""],
        capture_output=True, text=True, timeout=30)
    return r.stdout.strip()


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    schema = {}
    for t in TABLES:
        raw = q(f"SELECT COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT "
                f"FROM information_schema.COLUMNS "
                f"WHERE TABLE_SCHEMA='acore_world' AND TABLE_NAME='{t}' "
                f"ORDER BY ORDINAL_POSITION")
        cols = []
        for line in raw.split("\n"):
            if not line.strip():
                continue
            parts = line.split("\t")
            cols.append({
                "name": parts[0],
                "type": parts[1] if len(parts) > 1 else "?",
                "nullable": parts[2] if len(parts) > 2 else "?",
            })
        schema[t] = cols
        print(f"  {t}: {len(cols)} columns")

    # Also check characters DB for bot state
    for t in ["characters", "idlebot_bots", "character_queststatus_rewarded"]:
        raw = q(f"SELECT COLUMN_NAME, COLUMN_TYPE "
                f"FROM information_schema.COLUMNS "
                f"WHERE TABLE_SCHEMA='acore_characters' AND TABLE_NAME='{t}' "
                f"ORDER BY ORDINAL_POSITION")
        cols = []
        for line in raw.split("\n"):
            if not line.strip():
                continue
            parts = line.split("\t")
            cols.append({"name": parts[0], "type": parts[1] if len(parts) > 1 else "?"})
        schema[f"chars.{t}"] = cols
        print(f"  chars.{t}: {len(cols)} columns")

    out = os.path.join(OUT_DIR, "schema.json")
    with open(out, "w") as f:
        json.dump(schema, f, indent=2)
    print(f"\nSchema saved to {out}")


if __name__ == "__main__":
    main()
