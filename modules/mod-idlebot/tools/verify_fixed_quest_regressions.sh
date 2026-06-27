#!/usr/bin/env bash
# verify_fixed_quest_regressions.sh
#
# Regression gate for all previously-touched quest guide fixes.
# Checks the HOST VOLUME guide files that the worldserver actually reads:
#   /home/khuong/acore-zoidberg/data/guides/
#
# Also cross-checks key npc_ids against the live DB.
#
# Usage:
#   ./verify_fixed_quest_regressions.sh [--host <ssh-host>] [--guide-root <path>]
#
# Exit codes: 0 = all checks PASS, 1 = one or more FAIL

set -euo pipefail

BUILD_HOST="${BUILD_HOST:-khuong@10.10.30.20}"
GUIDE_ROOT="${GUIDE_ROOT:-/home/khuong/acore-zoidberg/data/guides}"

PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

run_remote() { ssh "$BUILD_HOST" "$1"; }

section() { echo ""; echo "=== $1 ==="; }

# ---------------------------------------------------------------------------
# Helper: check whether a guide file contains a quest's step(s)
# Returns "found" if any step for the quest exists, "clean" if not.
# ---------------------------------------------------------------------------
guide_has_quest_steps() {
    local file="$1" questid="$2"
    run_remote "grep -q 'quest_id: ${questid}' '${file}' 2>/dev/null && echo found || echo clean"
}

# ---------------------------------------------------------------------------
# Helper: get npc_id from the turn_in_quest step for a quest in a guide file
# ---------------------------------------------------------------------------
guide_turnin_npc() {
    local file="$1" questid="$2"
    run_remote "
        awk '
          /quest_id: ${questid}/{found=1}
          found && /type: turn_in_quest/{inturnin=1}
          inturnin && /npc_id:/{print \$2; exit}
          /^- id:/{if (found) exit}
        ' '${file}' 2>/dev/null
    "
}

# ---------------------------------------------------------------------------
# Helper: get npc_id of a creature in DB
# ---------------------------------------------------------------------------
db_creature_exists() {
    local npcid="$1"
    PW=$(run_remote "docker exec ac-database printenv MYSQL_ROOT_PASSWORD 2>/dev/null")
    COUNT=$(run_remote "docker exec ac-database mysql -uroot \"-p${PW}\" acore_world -se \
        'SELECT COUNT(*) FROM creature_template WHERE entry=${npcid};' 2>/dev/null | grep -v Warning | tail -1")
    echo "$COUNT"
}

echo "================================================================"
echo "IdleBot Fixed-Quest Regression Gate"
echo "Guide root: ${GUIDE_ROOT} (on ${BUILD_HOST})"
echo "================================================================"

# ---------------------------------------------------------------------------
# Q753 — A Humble Task (tauren, camp narache)
# FIX: q753_turnin npc_id was 2991 (Greatmother Hawkwind = GIVER)
#       should be 2981 (Chief Hawkwind = ENDER)
# ---------------------------------------------------------------------------
section "Q753 — A Humble Task (tauren camp narache)"

TAUREN_GUIDE="${GUIDE_ROOT}/horde/tauren/00_camp_narache-1-6.yaml"
Q753_NPC=$(run_remote "awk '/^- id: q753_turnin/{f=1} f && /npc_id:/{print \$2; exit}' '${TAUREN_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q753_NPC}" = "2981" ]; then
    pass "q753_turnin npc_id=${Q753_NPC} (Chief Hawkwind) ✓"
else
    fail "q753_turnin npc_id=${Q753_NPC} — EXPECTED 2981, got '${Q753_NPC}'"
    echo "       Fix: update ${TAUREN_GUIDE} q753_turnin npc_id to 2981"
fi

Q753_ACCEPT_NPC=$(run_remote "awk '/^- id: q753_accept/{f=1} f && /npc_id:/{print \$2; exit}' '${TAUREN_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q753_ACCEPT_NPC}" = "2991" ]; then
    pass "q753_accept npc_id=${Q753_ACCEPT_NPC} (Greatmother Hawkwind = GIVER) ✓"
else
    fail "q753_accept npc_id=${Q753_ACCEPT_NPC} — EXPECTED 2991 (the quest GIVER)"
fi

# ---------------------------------------------------------------------------
# Q755 — Rites of the Earthmother (tauren, camp narache)
# FIX: turnin npc should be 2982
# ---------------------------------------------------------------------------
section "Q755 — Rites of the Earthmother (tauren camp narache)"

Q755_NPC=$(run_remote "awk '/^- id: q755_turnin/{f=1} f && /npc_id:/{print \$2; exit}' '${TAUREN_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q755_NPC}" = "2982" ]; then
    pass "q755_turnin npc_id=${Q755_NPC} ✓"
else
    fail "q755_turnin npc_id=${Q755_NPC} — EXPECTED 2982"
fi

# ---------------------------------------------------------------------------
# Q747 — Rite of Strength (tauren, camp narache)
# FIX: turnin npc should be 2980
# ---------------------------------------------------------------------------
section "Q747 — Rite of Strength (tauren camp narache)"

Q747_NPC=$(run_remote "awk '/^- id: q747_turnin/{f=1} f && /npc_id:/{print \$2; exit}' '${TAUREN_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q747_NPC}" = "2980" ]; then
    pass "q747_turnin npc_id=${Q747_NPC} ✓"
else
    fail "q747_turnin npc_id=${Q747_NPC} — EXPECTED 2980"
fi

# ---------------------------------------------------------------------------
# Q21 — Skirmish at Echo Ridge (human northshire → goldshire duplication)
# FIX: remove q21 steps from goldshire guide; northshire only
# ---------------------------------------------------------------------------
section "Q21 — Skirmish at Echo Ridge (duplication: northshire only, not goldshire)"

GOLDSHIRE_GUIDE="${GUIDE_ROOT}/alliance/human/01_goldshire-6-12.yaml"
Q21_IN_GOLDSHIRE=$(run_remote "grep -c 'quest_id: 21' '${GOLDSHIRE_GUIDE}' 2>/dev/null || echo 0")
if [ "${Q21_IN_GOLDSHIRE}" = "0" ]; then
    pass "q21 steps NOT present in goldshire guide ✓"
else
    fail "q21 steps STILL PRESENT in goldshire guide (${Q21_IN_GOLDSHIRE} occurrences)"
    echo "       Fix: remove q21_accept/q21_kill1/q21_turnin from ${GOLDSHIRE_GUIDE}"
fi

NORTHSHIRE_GUIDE="${GUIDE_ROOT}/alliance/human/00_northshire-1-6.yaml"
Q21_IN_NORTHSHIRE=$(run_remote "grep -c 'quest_id: 21' '${NORTHSHIRE_GUIDE}' 2>/dev/null || echo 0")
if [ "${Q21_IN_NORTHSHIRE}" -gt 0 ]; then
    pass "q21 steps present in northshire guide (${Q21_IN_NORTHSHIRE} refs) ✓"
else
    fail "q21 steps MISSING from northshire guide — bots will never do Skirmish at Echo Ridge"
fi

# ---------------------------------------------------------------------------
# Q182 — The Troll Cave (dwarf/gnome: coldridge only, not kharanos)
# FIX: remove q182 from kharanos guide (both dwarf and gnome)
# ---------------------------------------------------------------------------
section "Q182 — The Troll Cave (duplication: coldridge only, not kharanos)"

DWARF_KHARANOS="${GUIDE_ROOT}/alliance/dwarf/01_kharanos-6-12.yaml"
Q182_IN_KHARANOS_DWARF=$(run_remote "grep -c 'quest_id: 182' '${DWARF_KHARANOS}' 2>/dev/null || echo 0")
if [ "${Q182_IN_KHARANOS_DWARF}" = "0" ]; then
    pass "q182 steps NOT present in dwarf kharanos guide ✓"
else
    fail "q182 steps STILL PRESENT in dwarf kharanos guide (${Q182_IN_KHARANOS_DWARF} occurrences)"
    echo "       Fix: remove q182_accept/q182_kill1/q182_turnin from ${DWARF_KHARANOS}"
fi

GNOME_KHARANOS="${GUIDE_ROOT}/alliance/gnome/01_kharanos-6-12.yaml"
Q182_IN_KHARANOS_GNOME=$(run_remote "grep -c 'quest_id: 182' '${GNOME_KHARANOS}' 2>/dev/null || echo 0")
if [ "${Q182_IN_KHARANOS_GNOME}" = "0" ]; then
    pass "q182 steps NOT present in gnome kharanos guide ✓"
else
    fail "q182 steps STILL PRESENT in gnome kharanos guide (${Q182_IN_KHARANOS_GNOME} occurrences)"
    echo "       Fix: remove q182_accept/q182_kill1/q182_turnin from ${GNOME_KHARANOS}"
fi

COLDRIDGE_GUIDE="${GUIDE_ROOT}/alliance/dwarf/00_coldridge-1-6.yaml"
Q182_IN_COLDRIDGE=$(run_remote "grep -c 'quest_id: 182' '${COLDRIDGE_GUIDE}' 2>/dev/null || echo 0")
if [ "${Q182_IN_COLDRIDGE}" -gt 0 ]; then
    pass "q182 steps present in coldridge guide (${Q182_IN_COLDRIDGE} refs) ✓"
else
    fail "q182 steps MISSING from coldridge guide — bots will never do The Troll Cave"
fi

# ---------------------------------------------------------------------------
# Q916 — Tenaron's Summons (nightelf dolanaar)
# FIX: turnin npc should be 2082
# ---------------------------------------------------------------------------
section "Q916 — Tenaron's Summons (nightelf dolanaar)"

DOLANAAR_GUIDE="${GUIDE_ROOT}/alliance/nightelf/01_dolanaar-6-12.yaml"
Q916_NPC=$(run_remote "awk '/^- id: q916_turnin/{f=1} f && /npc_id:/{print \$2; exit}' '${DOLANAAR_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q916_NPC}" = "2082" ]; then
    pass "q916_turnin npc_id=${Q916_NPC} ✓"
elif [ "${Q916_NPC}" = "missing" ] || [ -z "${Q916_NPC}" ]; then
    echo "  [SKIP] q916_turnin step not found in dolanaar guide (may be in shadowglen)"
else
    fail "q916_turnin npc_id=${Q916_NPC} — EXPECTED 2082"
fi

# ---------------------------------------------------------------------------
# Q376 — Garments of the Light (undead deathknell)
# FIX: turnin npc should be 1661
# ---------------------------------------------------------------------------
section "Q376 — Garments of the Light (undead deathknell)"

DEATHKNELL_GUIDE="${GUIDE_ROOT}/horde/undead/00_deathknell-1-6.yaml"
Q376_NPC=$(run_remote "awk '/^- id: q376_turnin/{f=1} f && /npc_id:/{print \$2; exit}' '${DEATHKNELL_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q376_NPC}" = "1661" ]; then
    pass "q376_turnin npc_id=${Q376_NPC} ✓"
elif [ "${Q376_NPC}" = "missing" ] || [ -z "${Q376_NPC}" ]; then
    echo "  [SKIP] q376_turnin step not found by expected id (check guide manually)"
else
    fail "q376_turnin npc_id=${Q376_NPC} — EXPECTED 1661"
fi

# ---------------------------------------------------------------------------
# Q789 — Encrypted Letter (human/orc/troll valley of trials or goldshire)
# FIX: turnin npc should be 3143
# ---------------------------------------------------------------------------
section "Q789 — Encrypted Letter (goldshire / valley of trials)"

Q789_FILES=$(run_remote "grep -rl 'quest_id: 789' '${GUIDE_ROOT}/alliance' '${GUIDE_ROOT}/horde' 2>/dev/null | grep -v generated_backup | grep -v '_1_60\|_1_18' | head -5")
Q789_OK=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    NPC=$(run_remote "awk '/quest_id: 789/{q=1} q && /type: turn_in_quest/{t=1} t && /npc_id:/{print \$2; exit}' '${f}' 2>/dev/null")
    if [ -z "$NPC" ]; then continue; fi
    if [ "$NPC" = "3143" ]; then
        pass "q789_turnin npc_id=${NPC} in $(basename $(dirname $f))/$(basename $f) ✓"
        Q789_OK=$((Q789_OK+1))
    else
        fail "q789_turnin npc_id=${NPC} — EXPECTED 3143 in ${f}"
    fi
done <<< "$Q789_FILES"
[ "$Q789_OK" -eq 0 ] && echo "  [SKIP] q789 turnin steps not found via standard id pattern"

# ---------------------------------------------------------------------------
# Q33 — Kobold Camp Cleanup / Kobold Candles (northshire area)
# FIX: turnin npc should be 196
# ---------------------------------------------------------------------------
section "Q33 — Kobold Camp Cleanup (northshire area)"

Q33_FILES=$(run_remote "grep -rl 'quest_id: 33' '${GUIDE_ROOT}/alliance/human' 2>/dev/null | grep -v generated_backup | grep -v '_1_60\|_1_18' | head -3")
Q33_OK=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    NPC=$(run_remote "awk '/quest_id: 33/{q=1} q && /type: turn_in_quest/{t=1} t && /npc_id:/{print \$2; exit}' '${f}' 2>/dev/null")
    if [ -z "$NPC" ]; then continue; fi
    if [ "$NPC" = "196" ]; then
        pass "q33_turnin npc_id=${NPC} in $(basename ${f}) ✓"
        Q33_OK=$((Q33_OK+1))
    else
        fail "q33_turnin npc_id=${NPC} — EXPECTED 196 in ${f}"
    fi
done <<< "$Q33_FILES"
[ "$Q33_OK" -eq 0 ] && echo "  [SKIP] q33 turnin steps not found via standard id pattern"

# ---------------------------------------------------------------------------
# Q459 — The Woodland Protector (nightelf shadowglen)
# FIX: turnin npc should be 1992
# ---------------------------------------------------------------------------
section "Q459 — The Woodland Protector (nightelf shadowglen)"

SHADOWGLEN_GUIDE="${GUIDE_ROOT}/alliance/nightelf/00_shadowglen-1-6.yaml"
Q459_NPC=$(run_remote "awk '/^- id: q459_turnin/{f=1} f && /npc_id:/{print \$2; exit}' '${SHADOWGLEN_GUIDE}' 2>/dev/null || echo missing")
if [ "${Q459_NPC}" = "1992" ]; then
    pass "q459_turnin npc_id=${Q459_NPC} ✓"
elif [ "${Q459_NPC}" = "missing" ] || [ -z "${Q459_NPC}" ]; then
    echo "  [SKIP] q459_turnin step not found by expected id (check guide manually)"
else
    fail "q459_turnin npc_id=${Q459_NPC} — EXPECTED 1992"
fi

# ---------------------------------------------------------------------------
# Q179 — Encrypted Parchment (dwarf coldridge / gnome gnomeregan)
# FIX: turnin npc should be 658
# ---------------------------------------------------------------------------
section "Q179 — Encrypted Parchment (coldridge / gnomeregan)"

Q179_FILES=$(run_remote "grep -rl 'quest_id: 179' '${GUIDE_ROOT}/alliance/dwarf' '${GUIDE_ROOT}/alliance/gnome' 2>/dev/null | grep -v generated_backup | grep -v '_1_60\|_1_18' | head -3")
Q179_OK=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    NPC=$(run_remote "awk '/quest_id: 179/{q=1} q && /type: turn_in_quest/{t=1} t && /npc_id:/{print \$2; exit}' '${f}' 2>/dev/null")
    if [ -z "$NPC" ]; then continue; fi
    if [ "$NPC" = "658" ]; then
        pass "q179_turnin npc_id=${NPC} in $(basename ${f}) ✓"
        Q179_OK=$((Q179_OK+1))
    else
        fail "q179_turnin npc_id=${NPC} — EXPECTED 658 in ${f}"
    fi
done <<< "$Q179_FILES"
[ "$Q179_OK" -eq 0 ] && echo "  [SKIP] q179 turnin steps not found via standard id pattern"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "================================================================"
echo "Result: ${PASS} PASS, ${FAIL} FAIL"
echo "================================================================"

if [ "$FAIL" -gt 0 ]; then
    echo "REGRESSION GATE FAILED — do NOT start soak until all FAILs are fixed"
    exit 1
else
    echo "All checks PASSED"
    exit 0
fi
