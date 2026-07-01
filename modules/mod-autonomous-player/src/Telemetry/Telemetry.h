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

#ifndef AUTONOMOUS_PLAYER_TELEMETRY_H
#define AUTONOMOUS_PLAYER_TELEMETRY_H

namespace AutonomousPlayer::Telemetry
{
    // All module logging goes through this category so it can be filtered
    // independently of every other module/subsystem in the log config.
    inline constexpr char const* LogCategory = "module.autonomous_player";
} // namespace AutonomousPlayer::Telemetry

#endif // AUTONOMOUS_PLAYER_TELEMETRY_H
