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

#ifndef AUTONOMOUS_PLAYER_BOT_NAVIGATION_H
#define AUTONOMOUS_PLAYER_BOT_NAVIGATION_H

class Player;

namespace AutonomousPlayer::Navigation
{
    // Walks `bot` toward (x, y, z) using the real movement generator
    // (Unit::GetMotionMaster()->MovePoint, generatePath=true -- real
    // navmesh pathing, the same mechanism a real player's client-driven
    // movement or any core NPC AI uses). This is the ONLY sanctioned way
    // for this module to change a bot's position: never TeleportTo /
    // NearTeleportTo / direct position setters (enforced by
    // tools/check_no_forbidden_apis.sh). Movement completes over
    // subsequent world ticks, same as it would for a real player -- this
    // function only issues the move request.
    void MoveTo(Player* bot, float x, float y, float z);
} // namespace AutonomousPlayer::Navigation

#endif // AUTONOMOUS_PLAYER_BOT_NAVIGATION_H
