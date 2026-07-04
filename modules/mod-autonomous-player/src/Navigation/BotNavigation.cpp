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

#include <cmath>

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
        // second follow-up): authored waypoint Zs are approximate and
        // a Z several yards off the surface reads as NOPATH. CRITICAL
        // detail (KNOWN_FAILURES.md #30's true root cause, found on
        // the 1->12 fleet runs): the probe must start only a LITTLE
        // above the requested Z. The old `z + 10` start reached above
        // the roof of any structure the destination sits inside (Den
        // burrow: NPC z 41.3, probe from 51.3 found the hilltop at
        // ~50; Camp Narache tent: vendor z 54, probe from 64 found
        // the tent roof at 63.9) and snapped the DESTINATION onto
        // that upper layer before pathing even began -- every
        // "arrived 2D-above the NPC" stranding traces here. With
        // +2.0f an exact NPC coordinate stays under its overhang
        // while approximate ground waypoints still normalize.
        float const groundZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), x, y, z + 2.0f, true);
        if (groundZ > INVALID_HEIGHT)
        {
            z = groundZ;
        }

        // A long leg whose target polygon exhausts the navmesh query
        // budget reads as NOPATH too -- but a real player just starts
        // WALKING toward a far destination. Bisect toward the target
        // (walking, never teleporting): full leg, then half, then
        // quarter. Each shorter leg re-normalizes Z along the line.
        float const sx = bot->GetPositionX();
        float const sy = bot->GetPositionY();
        float const sz = bot->GetPositionZ();
        for (float frac : { 1.0f, 0.5f, 0.25f })
        {
            float const tx = sx + (x - sx) * frac;
            float const ty = sy + (y - sy) * frac;
            float tz = sz + (z - sz) * frac;
            if (frac < 1.0f)
            {
                float const legZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), tx, ty, tz + 5.0f, true);
                if (legZ > INVALID_HEIGHT)
                {
                    tz = legZ;
                }
            }

            PathGenerator probe(bot);
            bool const found = probe.CalculatePath(tx, ty, tz, /*forceDest=*/false);
            if (!found || (probe.GetPathType() & PATHFIND_NOPATH) || probe.GetPath().size() < 2)
            {
                continue;
            }

            G3D::Vector3 const& end = probe.GetActualEndPosition();
            constexpr uint32 MovePointId = 0;
            bot->GetMotionMaster()->MovePoint(MovePointId, end.x, end.y, end.z,
                FORCED_MOVEMENT_NONE, 0.f, 0.f, /*generatePath=*/true, /*forceDestination=*/false);
            return;
        }

        // Direct line fully unpathable (a mesa lip, a cliff edge): a
        // real player walks somewhere ELSE and re-paths. Try eight
        // compass points 30yd out; any reachable one changes the next
        // probe's geometry. Only if every direction is NOPATH does the
        // bot genuinely stand still and let the ADR-028 bounds fire.
        for (int i = 0; i < 8; ++i)
        {
            float const angle = i * static_cast<float>(M_PI) / 4.0f;
            float const tx = sx + 30.0f * std::cos(angle);
            float const ty = sy + 30.0f * std::sin(angle);
            float tz = sz;
            float const legZ = bot->GetMap()->GetHeight(bot->GetPhaseMask(), tx, ty, tz + 5.0f, true);
            if (legZ > INVALID_HEIGHT)
            {
                tz = legZ;
            }

            PathGenerator probe(bot);
            bool const found = probe.CalculatePath(tx, ty, tz, /*forceDest=*/false);
            if (!found || (probe.GetPathType() & PATHFIND_NOPATH) || probe.GetPath().size() < 2)
            {
                continue;
            }

            G3D::Vector3 const& end = probe.GetActualEndPosition();
            constexpr uint32 MovePointId = 0;
            bot->GetMotionMaster()->MovePoint(MovePointId, end.x, end.y, end.z,
                FORCED_MOVEMENT_NONE, 0.f, 0.f, /*generatePath=*/true, /*forceDestination=*/false);
            return;
        }
    }
} // namespace AutonomousPlayer::Navigation
