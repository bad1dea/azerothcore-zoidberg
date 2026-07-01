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

#ifndef AUTONOMOUS_PLAYER_PERCEPTION_BUILDER_H
#define AUTONOMOUS_PLAYER_PERCEPTION_BUILDER_H

#include "PerceptionSnapshot.h"

class Player;

namespace AutonomousPlayer
{
    // Builds a PerceptionSnapshot from a live Player, synchronously, on the
    // world thread. The returned value must not be retained past the current
    // tick (see ADR-002 in ARCHITECTURE.md) -- callers own a copy, not a
    // reference into anything live.
    PerceptionSnapshot BuildPerceptionSnapshot(Player const* player);
} // namespace AutonomousPlayer

#endif // AUTONOMOUS_PLAYER_PERCEPTION_BUILDER_H
