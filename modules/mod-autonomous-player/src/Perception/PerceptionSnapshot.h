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

#ifndef AUTONOMOUS_PLAYER_PERCEPTION_SNAPSHOT_H
#define AUTONOMOUS_PLAYER_PERCEPTION_SNAPSHOT_H

#include "ObjectGuid.h"
#include <cstdint>
#include <string>

namespace AutonomousPlayer
{
    // A tick-safe, value-type view of a bot's world state.
    //
    // Deliberately contains no Player*/Unit*/Creature* or any other engine
    // pointer: it is built fresh from the resolved Player each tick (see
    // ARCHITECTURE.md ADR-002) and its lifetime never outlives that tick's
    // call stack. Planner/Executor/etc. consume it by value or const ref and
    // must never store it past the tick in which it was produced.
    struct PerceptionSnapshot
    {
        ObjectGuid CharacterGuid;
        std::string CharacterName;
        uint32_t Level = 0;
        uint32_t MapId = 0;
        float PositionX = 0.0f;
        float PositionY = 0.0f;
        float PositionZ = 0.0f;
        float Orientation = 0.0f;
        uint32_t Health = 0;
        uint32_t MaxHealth = 0;
        bool IsAlive = false;
        bool IsInCombat = false;
    };
} // namespace AutonomousPlayer

#endif // AUTONOMOUS_PLAYER_PERCEPTION_SNAPSHOT_H
