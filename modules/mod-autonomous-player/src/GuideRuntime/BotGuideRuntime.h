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
    // requirement). Slice 2 added StepType::KillNearest. Slice 3 adds
    // AcceptQuest/TurnInQuest, composing the already-proven
    // Navigation/Combat/Inventory/QuestEngine primitives (no new opcode
    // work) -- together these let a single guide run a full
    // accept-kill-turn-in quest loop with zero manual commands (ADR-021).
    // No guide authoring format, persistence, or step-failure-recovery
    // yet -- see ADR-019/ADR-020/ADR-021.
    enum class StepType : uint8_t
    {
        MoveTo,
        KillNearest,
        AcceptQuest,
        TurnInQuest,
    };

    // Shared sub-phase for any step that needs to walk to an NPC/creature
    // before acting on it (every step type except MoveTo, which has no
    // separate "act" -- arriving *is* the action). `Looting` is only ever
    // reached by KillNearest.
    enum class StepPhase : uint8_t
    {
        Approaching,
        Acting,
        Looting,
    };

    struct GuideStep
    {
        StepType Type = StepType::MoveTo;
        float X = 0.0f;                  // MoveTo target position
        float Y = 0.0f;
        float Z = 0.0f;
        uint32_t CreatureEntry = 0;       // KillNearest target entry, or AcceptQuest/TurnInQuest questgiver entry
        float SearchRadius = 100.0f;      // FindNearestCreature range
        uint32_t QuestId = 0;             // AcceptQuest / TurnInQuest
        uint32_t RewardChoiceIndex = 0;   // TurnInQuest
    };

    // Per-bot progress through a guide. Deliberately a plain value struct
    // (ADR-002's tick-safety rule) owned by the caller (BotLifecycleMgr),
    // not by GuideRuntime itself. `CurrentTargetGuid` is a GUID, never a
    // raw pointer, resolved fresh every tick -- same tick-safety rule.
    struct BotGuideState
    {
        std::vector<GuideStep> Steps;
        std::size_t CurrentStep = 0;
        bool ActionIssuedForCurrentStep = false;
        bool Finished = false;
        StepPhase CurrentPhase = StepPhase::Approaching;
        ObjectGuid CurrentTargetGuid;
    };

    // How close (yards) counts as "arrived" for a MoveTo step.
    inline constexpr float ArrivalToleranceYards = 3.0f;

    // Real quest interaction requires much closer range than a generic
    // "arrived" check -- confirmed this arc (Gate 2 QuestEngine slice):
    // an accept attempt at ~8.6 yards silently failed, ~1-2 yards
    // succeeded. Kept tighter than ArrivalToleranceYards deliberately.
    inline constexpr float InteractionToleranceYards = 2.0f;

    // Called once per bot per BotLifecycleMgr tick interval (see
    // BotLifecycleMgr::TickIntervalMs). Issues the current step's action
    // via the already-proven Navigation primitive if not already issued,
    // checks for completion, and advances `state` to the next step when
    // done -- entirely automatically, no debug command needed once a
    // guide has been started.
    void Tick(Player* bot, BotGuideState& state);
} // namespace AutonomousPlayer::GuideRuntime

#endif // AUTONOMOUS_PLAYER_BOT_GUIDE_RUNTIME_H
