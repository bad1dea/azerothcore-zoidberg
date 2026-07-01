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

#include "ObjectGuid.h"
#include <cstddef>
#include <cstdint>
#include <vector>

class Player;

namespace AutonomousPlayer::GuideRuntime
{
    // Gate 3 slice 1 added StepType::MoveTo -- the smallest possible
    // proof that a bot can advance through multiple steps with NO manual
    // command between them (Gate 3's "no manual step advances"
    // requirement). Slice 2 adds StepType::KillNearest, composing the
    // already-proven Navigation/Combat/Inventory primitives (no new
    // opcode work) through its own internal sub-phase (see KillPhase
    // below) since "walk to + attack + loot" isn't a single fire-and-
    // check action like MoveTo. No guide authoring format, persistence,
    // or step-failure-recovery yet -- see ADR-019/ADR-020.
    enum class StepType : uint8_t
    {
        MoveTo,
        KillNearest,
    };

    // Sub-phase for a KillNearest step -- irrelevant for MoveTo steps.
    enum class KillPhase : uint8_t
    {
        Approaching,
        Attacking,
        Looting,
    };

    struct GuideStep
    {
        StepType Type = StepType::MoveTo;
        float X = 0.0f;                // MoveTo target position
        float Y = 0.0f;
        float Z = 0.0f;
        uint32_t CreatureEntry = 0;     // KillNearest target creature entry
        float SearchRadius = 100.0f;    // KillNearest FindNearestCreature range
    };

    // Per-bot progress through a guide. Deliberately a plain value struct
    // (ADR-002's tick-safety rule) owned by the caller (BotLifecycleMgr),
    // not by GuideRuntime itself. `CurrentKillTarget` is a GUID, never a
    // raw pointer, resolved fresh every tick -- same tick-safety rule.
    struct BotGuideState
    {
        std::vector<GuideStep> Steps;
        std::size_t CurrentStep = 0;
        bool ActionIssuedForCurrentStep = false;
        bool Finished = false;
        KillPhase CurrentKillPhase = KillPhase::Approaching;
        ObjectGuid CurrentKillTarget;
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
