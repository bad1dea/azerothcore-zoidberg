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
    // No guide authoring format or persistence yet -- see
    // ADR-019/ADR-020/ADR-021. KillNearest's step-failure-recovery
    // arrived in ADR-022 (see PullState below).
    enum class StepType : uint8_t
    {
        MoveTo,
        KillNearest,
        AcceptQuest,
        TurnInQuest,
    };

    // Shared sub-phase for any non-combat step that needs to walk to an
    // NPC before acting on it (MoveTo has no separate "act" -- arriving
    // *is* the action; KillNearest uses its own PullState below instead,
    // since combat has real risk/confirmation complexity that quest
    // interaction doesn't).
    enum class StepPhase : uint8_t
    {
        Approaching,
        Acting,
        Looting,
    };

    // KillNearest's pull-transaction state, following the "select ->
    // validate -> approach -> open -> confirm engagement -> combat ->
    // finish -> loot" model in HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md
    // (ADR-022). The document's AssessRisk/PlanApproach/Prepare/
    // Stabilize/Recover stages have no real behavior yet at this
    // project's current maturity -- deliberately not modeled as separate
    // states until there's real logic to put in them (an empty
    // pass-through state is complexity with no payoff). What *is* new and
    // real here: `Approaching` is bounded by `MaxApproachTicks` and a
    // per-guide-step blacklist, so a target that never confirms
    // engagement is abandoned and retargeted instead of retried forever
    // -- the concrete gap this project's own `KillNearest` investigation
    // found (see KNOWN_FAILURES.md #3) and the research document's
    // "Failure handling and observability" section calls for explicitly.
    enum class PullState : uint8_t
    {
        Selecting,     // no live target yet; searching (excludes blacklisted guids)
        Approaching,   // target found; requesting the real attack every tick until IsInCombat() confirms it, or the bound expires
        Engaged,       // authoritative acknowledgement received (IsInCombat()); waiting for the target to die
        Looting,       // target confirmed dead; looting its corpse
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
        // KillNearest only (ADR-029): if nonzero, tried once per tick
        // during `Engaged` via `Combat::RequestCastSpell`, in addition to
        // the bare melee autoattack `RequestAttack` already provides.
        // Real resource/cooldown/range requirements apply for real (see
        // ADR-018) -- failing (e.g. not enough rage yet) is a harmless,
        // expected no-op, not an error; RequestAttack keeps the melee
        // swing going regardless. This is deliberately the smallest
        // possible "class controller" step: use one real learned ability
        // opportunistically, not a priority list, resource tracking, or
        // ability rotation.
        uint32_t OpportunisticSpellId = 0;
    };

    // Per-bot progress through a guide. Deliberately a plain value struct
    // (ADR-002's tick-safety rule) owned by the caller (BotLifecycleMgr),
    // not by GuideRuntime itself. `CurrentTargetGuid` is a GUID, never a
    // raw pointer, resolved fresh every tick -- same tick-safety rule.
    // `CurrentPullState`/`ApproachTicks`/`BlacklistedTargets` are
    // KillNearest-specific (ADR-022); harmless no-ops for other step
    // types, which use `CurrentPhase` instead. `OperationTicks`/`Failed`
    // are shared, generic bounded-wait bookkeeping (ADR-028) -- used by
    // any step/phase that would otherwise wait indefinitely for a
    // condition that might never become true (a target NPC/creature that
    // was never reachable, a quest request that never resolves, every
    // candidate blacklisted with nothing left to try).
    struct BotGuideState
    {
        std::vector<GuideStep> Steps;
        std::size_t CurrentStep = 0;
        bool ActionIssuedForCurrentStep = false;
        bool Finished = false;
        bool Failed = false;
        StepPhase CurrentPhase = StepPhase::Approaching;
        ObjectGuid CurrentTargetGuid;
        PullState CurrentPullState = PullState::Selecting;
        uint32_t ApproachTicks = 0;
        std::vector<ObjectGuid> BlacklistedTargets;
        uint32_t OperationTicks = 0;

        // Loot verification (ADR-030): whether the most recent
        // `KillNearest` loot attempt was actually confirmed to have
        // taken everything that was there, checked via real before/after
        // state on the corpse itself -- not assumed just because
        // `Inventory::LootCorpse` was called. `LastLootAttempted` is
        // false if the corpse could not even be resolved to attempt
        // looting (e.g. already despawned).
        bool LastLootAttempted = false;
        bool LastLootVerified = false;
    };

    // How close (yards) counts as "arrived" for a MoveTo step.
    inline constexpr float ArrivalToleranceYards = 3.0f;

    // Real quest interaction requires much closer range than a generic
    // "arrived" check -- confirmed this arc (Gate 2 QuestEngine slice):
    // an accept attempt at ~8.6 yards silently failed, ~1-2 yards
    // succeeded. Kept tighter than ArrivalToleranceYards deliberately.
    inline constexpr float InteractionToleranceYards = 2.0f;

    // How many ticks (BotLifecycleMgr::TickIntervalMs each, ~1s) to keep
    // requesting an attack on a target before giving up and blacklisting
    // it. Both of today's real, clean `KillNearest` completions finished
    // in 12-15 seconds; 20 gives real margin above that observed range
    // without letting a genuinely unreachable target stall indefinitely.
    inline constexpr uint32_t MaxApproachTicks = 20;

    // Generic bound (ADR-028) for any other wait that previously had
    // none at all: `MoveTo`'s arrival wait, `AcceptQuest`/`TurnInQuest`'s
    // NPC-search-and-walk wait and their request-retry wait, and
    // `KillNearest`'s `Selecting`/`Engaged` waits (nothing found/
    // everything blacklisted; no combat deadline). Deliberately more
    // generous than `MaxApproachTicks`. NOTE: verified live that the
    // real-world tick rate is faster than the naive "~1 tick/second"
    // assumption (observed ~2.3 ticks/real-second) -- 45 ticks bounds a
    // wait to roughly 20 real seconds in practice, not ~45. The bound
    // itself is safe either way (a shorter real timeout than intended is
    // not a correctness problem); this note exists so the number isn't
    // misread as "~45 seconds" again.
    inline constexpr uint32_t MaxOperationTicks = 45;

    // Called once per bot per BotLifecycleMgr tick interval (see
    // BotLifecycleMgr::TickIntervalMs). Issues the current step's action
    // via the already-proven Navigation primitive if not already issued,
    // checks for completion, and advances `state` to the next step when
    // done -- entirely automatically, no debug command needed once a
    // guide has been started.
    void Tick(Player* bot, BotGuideState& state);
} // namespace AutonomousPlayer::GuideRuntime

#endif // AUTONOMOUS_PLAYER_BOT_GUIDE_RUNTIME_H
