#!/usr/bin/env python3
"""Parse HonorBuddy Profile v3 XML files for quest ordering + metadata.

Extracts quest sequences, vendor locations, blackspots, transport routes,
and per-zone settings. Validates quest IDs against our DB data.

Usage: python3 parse_honorbuddy_profiles.py --input /path/to/Profile\ v3 --out ../../data/generated
"""

import xml.etree.ElementTree as ET
import json, os, sys, re, argparse, glob

def parse_profile(filepath):
    """Parse a single HB profile XML and extract all useful data."""
    try:
        tree = ET.parse(filepath)
    except ET.ParseError as e:
        print(f"  WARN: parse error in {filepath}: {e}")
        return None

    root = tree.getroot()
    profile = {
        "file": os.path.basename(filepath),
        "quests": [],
        "vendors": [],
        "blackspots": [],
        "hotspots": [],
        "settings": {},
        "custom_behaviors": [],
        "conditions": [],
        "load_profile": None,
    }

    # Extract settings from top-level attributes or CustomBehavior UserSettings
    for attr in ["MinDurability", "MinFreeBagSlots", "SellGrey", "SellWhite",
                 "SellGreen", "MailGrey", "MailWhite", "MailGreen", "MailBlue",
                 "MailPurple"]:
        val = root.get(attr)
        if val is not None:
            profile["settings"][attr] = val

    # Walk all elements
    for elem in root.iter():
        tag = elem.tag.split("}")[-1] if "}" in elem.tag else elem.tag

        # Quest pickup
        if tag == "PickUp":
            qid = elem.get("QuestId") or elem.get("QuestName")
            if qid and qid.isdigit():
                profile["quests"].append({"action": "pickup", "quest_id": int(qid),
                    "npc_id": int(elem.get("GiverIdOrEntry") or elem.get("GiverId") or 0),
                    "x": float(elem.get("X") or 0), "y": float(elem.get("Y") or 0), "z": float(elem.get("Z") or 0)})

        # Quest turn-in
        elif tag == "TurnIn":
            qid = elem.get("QuestId") or elem.get("QuestName")
            if qid and qid.isdigit():
                profile["quests"].append({"action": "turnin", "quest_id": int(qid),
                    "npc_id": int(elem.get("TurnInIdOrEntry") or elem.get("TurnInId") or 0),
                    "x": float(elem.get("X") or 0), "y": float(elem.get("Y") or 0), "z": float(elem.get("Z") or 0)})

        # Quest objective
        elif tag == "Objective":
            qid = elem.get("QuestId")
            if qid and qid.isdigit():
                entry = {"action": "objective", "quest_id": int(qid), "hotspots": []}
                for hs in elem.findall(".//Hotspot"):
                    entry["hotspots"].append({
                        "x": float(hs.get("X") or 0), "y": float(hs.get("Y") or 0), "z": float(hs.get("Z") or 0)
                    })
                profile["quests"].append(entry)

        # Vendor entries
        elif tag == "Vendor":
            profile["vendors"].append({
                "entry": int(elem.get("Entry") or elem.get("Id") or 0),
                "name": elem.get("Name", ""),
                "type": elem.get("Type", "Repair"),
                "x": float(elem.get("X") or 0), "y": float(elem.get("Y") or 0), "z": float(elem.get("Z") or 0),
            })
        elif tag == "VendorEntry" or (tag == "CustomBehavior" and elem.get("File") == "Vendor"):
            entry = int(elem.get("Entry") or elem.get("Id") or 0)
            if entry:
                profile["vendors"].append({"entry": entry,
                    "x": float(elem.get("X") or 0), "y": float(elem.get("Y") or 0), "z": float(elem.get("Z") or 0)})

        # Blackspots
        elif tag == "Blackspot":
            profile["blackspots"].append({
                "x": float(elem.get("X") or 0), "y": float(elem.get("Y") or 0), "z": float(elem.get("Z") or 0),
                "radius": float(elem.get("Radius") or 5),
            })

        # Hotspots (standalone, not in objectives)
        elif tag == "Hotspot" and elem.getparent() is None if hasattr(elem, 'getparent') else True:
            x, y, z = float(elem.get("X") or 0), float(elem.get("Y") or 0), float(elem.get("Z") or 0)
            if x != 0 or y != 0:
                profile["hotspots"].append({"x": x, "y": y, "z": z})

        # CustomBehaviors
        elif tag == "CustomBehavior":
            cb = {"file": elem.get("File", ""), "attrs": dict(elem.attrib)}
            # Extract transport data
            if cb["file"] == "UseTransport":
                cb["transport"] = {
                    "transport_id": int(elem.get("TransportId") or 0),
                    "wait_at": {"x": float(elem.get("WaitAtX") or 0), "y": float(elem.get("WaitAtY") or 0), "z": float(elem.get("WaitAtZ") or 0)},
                    "stand_on": {"x": float(elem.get("StandOnX") or 0), "y": float(elem.get("StandOnY") or 0), "z": float(elem.get("StandOnZ") or 0)},
                    "get_off": {"x": float(elem.get("GetOffX") or 0), "y": float(elem.get("GetOffY") or 0), "z": float(elem.get("GetOffZ") or 0)},
                    "start_loc": {"x": float(elem.get("TransportStartX") or elem.get("StartX") or 0),
                                  "y": float(elem.get("TransportStartY") or elem.get("StartY") or 0),
                                  "z": float(elem.get("TransportStartZ") or elem.get("StartZ") or 0)},
                    "end_loc": {"x": float(elem.get("TransportEndX") or elem.get("EndX") or 0),
                                "y": float(elem.get("TransportEndY") or elem.get("EndY") or 0),
                                "z": float(elem.get("TransportEndZ") or elem.get("EndZ") or 0)},
                }
            elif cb["file"] == "UserSettings":
                for k, v in elem.attrib.items():
                    if k != "File":
                        profile["settings"][k] = v
            elif cb["file"] == "RunTo":
                cb["coords"] = {"x": float(elem.get("X") or 0), "y": float(elem.get("Y") or 0), "z": float(elem.get("Z") or 0)}
            profile["custom_behaviors"].append(cb)

        # LoadProfile (chain to next)
        elif tag == "LoadProfile":
            profile["load_profile"] = elem.get("ProfileName", "")

        # If conditions
        elif tag == "If":
            cond = elem.get("Condition", "")
            if cond:
                profile["conditions"].append(cond)

    return profile


def categorize_profiles(profiles):
    """Group profiles by faction/race/level range."""
    categorized = {
        "alliance": {},
        "horde": {},
        "transport": [],
    }

    race_map = {
        "Human": "human", "Dwarf": "dwarf", "Gnome": "gnome",
        "Night Elf": "nightelf", "NightElf": "nightelf", "Draenei": "draenei",
        "Orc": "orc", "Troll": "troll", "Undead": "undead",
        "Tauren": "tauren", "Blood Elf": "bloodelf", "BloodElf": "bloodelf",
    }

    for p in profiles:
        fn = p["file"]
        fp = p.get("full_path", fn)

        # Transport profiles
        if "transport" in fn.lower() or "/Transport/" in fp or "\\Transport\\" in fp:
            categorized["transport"].append(p)
            continue

        # Detect faction from directory path OR filename
        faction = None
        if "/Alliance/" in fp or "\\Alliance\\" in fp or fn.startswith("[A"):
            faction = "alliance"
        elif "/Horde/" in fp or "\\Horde\\" in fp or fn.startswith("[H"):
            faction = "horde"

        # Detect race from filename
        race = None
        fn_lower = fn.lower()
        for rn, rk in race_map.items():
            if rn.lower() in fn_lower:
                race = rk
                break
        # Handle combined "Dwarf & Gnome" profiles
        if "dwarf" in fn_lower and "gnome" in fn_lower:
            race = "dwarf"  # primary; gnome shares the same zone
        if "orc" in fn_lower and "troll" in fn_lower:
            race = "orc"  # primary; troll shares Valley of Trials

        # Detect level range from filename
        level_matches = re.findall(r"\((\d+)-(\d+)\)", fn)
        if level_matches:
            # Use the LAST match (usually the actual level range, not the category)
            level_min = int(level_matches[-1][0])
            level_max = int(level_matches[-1][1])
        else:
            level_min, level_max = 1, 12

        if faction and race:
            if race not in categorized[faction]:
                categorized[faction][race] = []
            p["level_min"] = level_min
            p["level_max"] = level_max
            categorized[faction][race].append(p)

            # Also add gnome/troll copies for shared starter zones
            if "gnome" in fn_lower and "dwarf" in fn_lower:
                if "gnome" not in categorized[faction]:
                    categorized[faction]["gnome"] = []
                categorized[faction]["gnome"].append(dict(p))
            if "troll" in fn_lower and "orc" in fn_lower:
                if "troll" not in categorized[faction]:
                    categorized[faction]["troll"] = []
                categorized[faction]["troll"].append(dict(p))
        elif faction:
            # Zone profile without specific race — shared zone
            key = f"shared_{level_min}_{level_max}"
            if key not in categorized[faction]:
                categorized[faction][key] = []
            p["level_min"] = level_min
            p["level_max"] = level_max
            categorized[faction][key].append(p)

    return categorized


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="Path to Profile v3 directory")
    parser.add_argument("--out", required=True, help="Output directory for JSON files")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    # Find all XML files
    xml_files = glob.glob(os.path.join(args.input, "**", "*.xml"), recursive=True)
    print(f"Found {len(xml_files)} XML profile files.")

    profiles = []
    for xf in sorted(xml_files):
        p = parse_profile(xf)
        if p:
            p["full_path"] = xf
            profiles.append(p)
            quest_count = len(p["quests"])
            if quest_count > 0:
                print(f"  {p['file']}: {quest_count} quest steps, {len(p['vendors'])} vendors, {len(p['blackspots'])} blackspots")

    categorized = categorize_profiles(profiles)

    # Save quest ordering hints
    hints = {}
    for faction in ["alliance", "horde"]:
        for race, profs in categorized[faction].items():
            key = f"{faction}/{race}"
            hints[key] = []
            for p in sorted(profs, key=lambda x: x.get("level_min", 0)):
                hints[key].append({
                    "file": p["file"],
                    "level_min": p.get("level_min", 1),
                    "level_max": p.get("level_max", 12),
                    "quest_order": [q["quest_id"] for q in p["quests"] if "quest_id" in q],
                    "settings": p["settings"],
                    "load_profile": p.get("load_profile"),
                })

    with open(os.path.join(args.out, "profile_hints_honorbuddy.json"), "w") as f:
        json.dump(hints, f, indent=2)
    print(f"\nSaved profile_hints_honorbuddy.json ({len(hints)} race/faction combos)")

    # Save vendor locations
    all_vendors = []
    for p in profiles:
        for v in p["vendors"]:
            if v.get("entry") or (v.get("x") and v.get("y")):
                v["source_file"] = p["file"]
                all_vendors.append(v)
    with open(os.path.join(args.out, "vendor_locations.json"), "w") as f:
        json.dump(all_vendors, f, indent=2)
    print(f"Saved vendor_locations.json ({len(all_vendors)} vendors)")

    # Save blackspots
    all_blackspots = []
    for p in profiles:
        for bs in p["blackspots"]:
            bs["source_file"] = p["file"]
            all_blackspots.append(bs)
    with open(os.path.join(args.out, "blackspots.json"), "w") as f:
        json.dump(all_blackspots, f, indent=2)
    print(f"Saved blackspots.json ({len(all_blackspots)} blackspots)")

    # Save transport routes
    transport_data = []
    for p in categorized.get("transport", []):
        for cb in p["custom_behaviors"]:
            if cb["file"] == "UseTransport" and "transport" in cb:
                t = cb["transport"]
                t["source_file"] = p["file"]
                transport_data.append(t)
        # Also save RunTo waypoints as travel routes
        run_tos = [cb for cb in p["custom_behaviors"] if cb["file"] == "RunTo" and "coords" in cb]
        if run_tos:
            transport_data.append({
                "type": "waypoint_chain",
                "source_file": p["file"],
                "waypoints": [cb["coords"] for cb in run_tos],
            })
    # Also extract transport from non-transport profiles
    for p in profiles:
        for cb in p["custom_behaviors"]:
            if cb["file"] == "UseTransport" and "transport" in cb:
                t = cb["transport"]
                t["source_file"] = p["file"]
                transport_data.append(t)

    with open(os.path.join(args.out, "transport_routes.json"), "w") as f:
        json.dump(transport_data, f, indent=2)
    print(f"Saved transport_routes.json ({len(transport_data)} transport entries)")


if __name__ == "__main__":
    main()
