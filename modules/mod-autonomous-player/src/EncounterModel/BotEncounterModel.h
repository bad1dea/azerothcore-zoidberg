/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AUTONOMOUS_PLAYER_BOT_ENCOUNTER_MODEL_H
#define AUTONOMOUS_PLAYER_BOT_ENCOUNTER_MODEL_H

#include "ObjectGuid.h"
#include <cstdint>
#include <vector>

class Player;

namespace AutonomousPlayer::EncounterModel
{
    // Gate 3 implementation sequence step 2 (ADR-022/024): the smallest
    // real, testable slice of the research document's `EncounterModel`
    // -- "what is actually attacking the bot right now," which
    // `GuideRuntime`'s `KillNearest` currently has zero awareness of (it
    // only ever tracks its own single objective target). No risk
    // scoring, line-of-sight, cast-tracking, or threat data yet --
    // deliberately deferred until there's a real consumer for them.
    struct Attacker
    {
        ObjectGuid Guid;
        uint32_t Entry = 0;
        float Distance = 0.0f;
        bool IsObjectiveTarget = false; // is this GuideRuntime's current KillNearest target
    };

    // Immutable per-tick snapshot (ADR-002 tick-safety: a plain value
    // type, never stores a Unit*/Creature* past the tick that built it),
    // built only from real, authoritative server mechanics
    // (`Unit::getAttackers()`, the engine's own live attacker-tracking
    // set) -- per the research document, "consumes only normal server
    // mechanics ... policy must not use hidden future spawn information
    // or teleport-like shortcuts."
    struct Snapshot
    {
        bool BotInCombat = false;
        std::vector<Attacker> Attackers;

        // True if something is attacking the bot that isn't the guide's
        // planned objective target -- an unplanned add. This is the
        // single most important fact this project's combat logic
        // currently has no way to know at all.
        [[nodiscard]] bool HasUnplannedAdd() const;
    };

    // Builds a snapshot for `bot` right now. `objectiveTarget` (pass
    // ObjectGuid::Empty if none) is only used to tag
    // `IsObjectiveTarget`/`HasUnplannedAdd` -- this function observes,
    // it does not decide anything.
    Snapshot BuildSnapshot(Player* bot, ObjectGuid const& objectiveTarget);
} // namespace AutonomousPlayer::EncounterModel

#endif // AUTONOMOUS_PLAYER_BOT_ENCOUNTER_MODEL_H
