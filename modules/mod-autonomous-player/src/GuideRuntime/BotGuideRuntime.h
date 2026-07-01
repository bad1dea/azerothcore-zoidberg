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

#ifndef AUTONOMOUS_PLAYER_BOT_GUIDE_RUNTIME_H
#define AUTONOMOUS_PLAYER_BOT_GUIDE_RUNTIME_H

#include <cstddef>
#include <cstdint>
#include <vector>

class Player;

namespace AutonomousPlayer::GuideRuntime
{
    // Gate 3 slice 1: the smallest possible proof that a bot can advance
    // through multiple steps with NO manual command between them (Gate
    // 3's "no manual step advances" requirement). Deliberately minimal:
    // a fixed, hardcoded list of waypoints (`StepType::MoveTo` only) --
    // no guide authoring/persistence/combat-in-guide/step-failure-
    // recovery yet, those are separate later slices (see ADR-019).
    enum class StepType : uint8_t
    {
        MoveTo,
    };

    struct GuideStep
    {
        StepType Type = StepType::MoveTo;
        float X = 0.0f;
        float Y = 0.0f;
        float Z = 0.0f;
    };

    // Per-bot progress through a guide. Deliberately a plain value struct
    // (ADR-002's tick-safety rule) owned by the caller (BotLifecycleMgr),
    // not by GuideRuntime itself.
    struct BotGuideState
    {
        std::vector<GuideStep> Steps;
        std::size_t CurrentStep = 0;
        bool ActionIssuedForCurrentStep = false;
        bool Finished = false;
    };

    // How close (yards) counts as "arrived" for a MoveTo step.
    inline constexpr float ArrivalToleranceYards = 3.0f;

    // Called once per bot per BotLifecycleMgr tick interval (see
    // BotLifecycleMgr::TickIntervalMs). Issues the current step's action
    // via the already-proven Navigation primitive if not already issued,
    // checks for completion, and advances `state` to the next step when
    // done -- entirely automatically, no debug command needed once a
    // guide has been started.
    void Tick(Player* bot, BotGuideState& state);
} // namespace AutonomousPlayer::GuideRuntime

#endif // AUTONOMOUS_PLAYER_BOT_GUIDE_RUNTIME_H
