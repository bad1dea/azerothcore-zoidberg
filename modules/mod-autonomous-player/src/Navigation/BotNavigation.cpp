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

#include "BotNavigation.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "Player.h"

namespace AutonomousPlayer::Navigation
{
    // KNOWN_FAILURES.md #14, root-caused by the user literally watching
    // a bot fly (2026-07-02): a bare MovePoint defaults to
    // forceDestination=true, and PointMovementGenerator's fallback for a
    // failed/NOPATH navmesh query is a RAW straight-line spline to the
    // literal destination -- a server-driven player character has no
    // client applying gravity, so an unreachable or badly-Z'd target
    // makes the bot visibly fly through the air and then hover wherever
    // the guide's bounded wait stopped it. Every MoveTo in the module
    // funnels through here, so this is the single choke point: probe the
    // navmesh first, walk only the reachable portion of the path, and
    // refuse to move at all when there is no path -- standing still
    // until the guide's ADR-028 bound fires is the correct, already-
    // designed outcome for an unreachable target; becoming airborne
    // never is.
    void MoveTo(Player* bot, float x, float y, float z)
    {
        if (!bot)
        {
            return;
        }

        // Normalize the requested Z to the real ground first (#14
        // second follow-up, same day): authored waypoint Zs are
        // approximate, and the navmesh query's vertical search extents
        // around the destination are narrow -- a Z several yards off
        // the actual surface (a spawn-table average on Teldrassil's
        // uneven canopy, live case: requested 1320.9, bot refused to
        // move at all) reads as NOPATH. The old airborne code masked
        // exactly this class of data error too, by flying to the
        // literal coordinates instead.
        float const groundZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, z + 10.0f, true);
        if (groundZ > INVALID_HEIGHT)
        {
            z = groundZ;
        }

        PathGenerator probe(bot);
        bool const found = probe.CalculatePath(x, y, z, /*forceDest=*/false);
        if (!found || (probe.GetPathType() & PATHFIND_NOPATH) || probe.GetPath().size() < 2)
        {
            return;
        }

        // The truncated-to-navmesh endpoint: for a fully reachable
        // destination this IS the destination; for a partially reachable
        // one it is the furthest grounded point toward it.
        G3D::Vector3 const& end = probe.GetActualEndPosition();

        constexpr uint32 MovePointId = 0;
        bot->GetMotionMaster()->MovePoint(MovePointId, end.x, end.y, end.z,
            FORCED_MOVEMENT_NONE, 0.f, 0.f, /*generatePath=*/true, /*forceDestination=*/false);
    }
} // namespace AutonomousPlayer::Navigation
