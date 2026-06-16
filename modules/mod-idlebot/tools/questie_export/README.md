# Questie export (offline tool — NOT runtime)

Parses Questie-style addon data into normalized JSON for guide authoring. Runs
OFFLINE; nothing here executes inside worldserver.

Outputs: quests.json, npcs.json, objects.json, items.json,
objective_locations.json, quest_starters.json, quest_enders.json,
guide_waypoints.json.

Rules: handle Lua tables carefully; preserve IDs; normalize coordinates;
cross-check against the AzerothCore world DB; flag mismatches; allow manual
overrides; never blindly trust addon data over server DB data.

Scripts (stubs): parse_questie_lua.py, normalize_questie_json.py.
