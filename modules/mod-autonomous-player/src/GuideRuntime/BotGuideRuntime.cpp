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

#include "BotGuideRuntime.h"
#include "Combat/BotCombat.h"
#include "Creature.h"
#include "Inventory/BotLoot.h"
#include "MotionMaster.h"
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestEngine/BotQuestEngine.h"

#include <algorithm>
#include <limits>
#include <list>

namespace AutonomousPlayer::GuideRuntime
{
    namespace
    {
        void AdvanceToNextStep(BotGuideState& state)
        {
            ++state.CurrentStep;
            state.ActionIssuedForCurrentStep = false;
            state.CurrentPhase = StepPhase::Approaching;
            state.CurrentTargetGuid = ObjectGuid::Empty;
            state.CurrentPullState = PullState::Selecting;
            state.ApproachTicks = 0;
            state.BlacklistedTargets.clear();
        }

        // Nearest live creature of `entry` within `range`, excluding any
        // guid in `blacklist` -- Player::FindNearestCreature has no
        // exclusion parameter, so this enumerates candidates directly
        // (same GetCreatureListWithEntryInGrid primitive the
        // .autonomousplayer multipull debug command already uses) and
        // picks the nearest non-blacklisted one manually.
        Creature* FindNearestNonBlacklisted(
            Player* bot, uint32_t entry, float range, std::vector<ObjectGuid> const& blacklist)
        {
            std::list<Creature*> candidates;
            bot->GetCreatureListWithEntryInGrid(candidates, entry, range);

            Creature* best = nullptr;
            float bestDistance = std::numeric_limits<float>::max();

            for (Creature* candidate : candidates)
            {
                if (!candidate->IsAlive())
                {
                    continue;
                }

                if (std::find(blacklist.begin(), blacklist.end(), candidate->GetGUID()) != blacklist.end())
                {
                    continue;
                }

                float distance = bot->GetDistance(candidate);
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = candidate;
                }
            }

            return best;
        }

        void TickMoveTo(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            if (!state.ActionIssuedForCurrentStep)
            {
                Navigation::MoveTo(bot, step.X, step.Y, step.Z);
                state.ActionIssuedForCurrentStep = true;
                return;
            }

            if (bot->GetDistance(step.X, step.Y, step.Z) <= ArrivalToleranceYards)
            {
                AdvanceToNextStep(state);
            }
        }

        // Walk to + attack + loot the nearest non-blacklisted creature of
        // a given entry, fully automatically -- composes the
        // already-proven Combat/Inventory primitives (no new opcode
        // work) through an explicit pull-transaction state machine
        // (`PullState`, ADR-022, following
        // HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md's "select -> approach ->
        // open -> confirm engagement -> combat -> finish -> loot" model).
        //
        // Opening with `Combat::RequestAttack` (not a bare movement
        // order) and confirming with `bot->IsInCombat()` is deliberate,
        // hard-won knowledge, not a stylistic choice -- see
        // KNOWN_FAILURES.md #3 for the full story: two earlier attempts
        // (gate on arrival before attacking; call a bare
        // `MotionMaster::MoveChase` with no attack request at all) were
        // each live-tested and each produced a permanent stall. Real
        // diagnostics proved a bare `MoveChase` produces *zero* bot
        // movement -- `Unit::Attack()` (via `HandleAttackSwingOpcode`,
        // inside `RequestAttack`) called *before* its own `MoveChase` is
        // required for the chase to actually engage. `RequestAttack` is
        // idempotent to re-issue every tick while approaching.
        //
        // New in this slice: `Approaching` is bounded by
        // `MaxApproachTicks`. A target that never confirms engagement in
        // time is blacklisted (scoped to this guide step, cleared on
        // `AdvanceToNextStep`) and a different candidate is selected --
        // closing the "retry forever" gap the research document's
        // failure-handling section calls out, instead of leaving it
        // implicit.
        void TickKillNearest(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            switch (state.CurrentPullState)
            {
                case PullState::Selecting:
                {
                    Creature* target = FindNearestNonBlacklisted(
                        bot, step.CreatureEntry, step.SearchRadius, state.BlacklistedTargets);
                    if (!target)
                    {
                        // Nothing available (or everything found so far
                        // is blacklisted) -- retry next tick.
                        return;
                    }

                    state.CurrentTargetGuid = target->GetGUID();
                    state.ApproachTicks = 0;
                    state.CurrentPullState = PullState::Approaching;
                    break;
                }

                case PullState::Approaching:
                {
                    Creature* target = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!target || !target->IsAlive())
                    {
                        // Died/despawned before we engaged -- it's simply
                        // gone, no need to blacklist a nonexistent guid.
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        state.CurrentPullState = PullState::Selecting;
                        return;
                    }

                    if (++state.ApproachTicks > MaxApproachTicks)
                    {
                        // Never confirmed engagement in a reasonable
                        // time -- give up on this target, blacklist it
                        // for the rest of this step, and pick a
                        // different one rather than retrying forever.
                        state.BlacklistedTargets.push_back(state.CurrentTargetGuid);
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        state.CurrentPullState = PullState::Selecting;
                        return;
                    }

                    Combat::RequestAttack(bot, state.CurrentTargetGuid);

                    if (bot->IsInCombat())
                    {
                        state.CurrentPullState = PullState::Engaged;
                    }

                    break;
                }

                case PullState::Engaged:
                {
                    Creature* target = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!target || !target->IsAlive())
                    {
                        state.CurrentPullState = PullState::Looting;
                    }

                    break;
                }

                case PullState::Looting:
                {
                    if (Creature* corpse = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid))
                    {
                        Inventory::LootCorpse(bot, corpse);
                    }

                    // Best-effort: whether or not the corpse was still
                    // resolvable (it may have already despawned), the
                    // step is done -- this slice doesn't retry a missed
                    // loot window.
                    AdvanceToNextStep(state);
                    break;
                }
            }
        }

        // Walk to a quest giver and accept a real quest, fully
        // automatically. Unlike KillNearest's attack, quest acceptance
        // requires genuine close interaction range (confirmed this arc:
        // ~8.6 yards silently failed, ~1-2 yards succeeded) -- so this
        // waits for real arrival (InteractionToleranceYards) before
        // submitting the request, rather than firing immediately like
        // TickKillNearest does for melee engagement.
        void TickAcceptQuest(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            switch (state.CurrentPhase)
            {
                case StepPhase::Approaching:
                {
                    if (state.CurrentTargetGuid.IsEmpty())
                    {
                        Creature* giver = bot->FindNearestCreature(step.CreatureEntry, step.SearchRadius, true);
                        if (!giver)
                        {
                            return;
                        }

                        state.CurrentTargetGuid = giver->GetGUID();
                        Navigation::MoveTo(bot, giver->GetPositionX(), giver->GetPositionY(), giver->GetPositionZ());
                        return;
                    }

                    Creature* giver = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!giver)
                    {
                        // Despawned before we arrived -- give up on this
                        // guid and retry the search next tick.
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        return;
                    }

                    if (bot->GetDistance(giver) <= InteractionToleranceYards)
                    {
                        state.CurrentPhase = StepPhase::Acting;
                    }

                    break;
                }

                case StepPhase::Acting:
                {
                    QuestEngine::RequestAcceptQuest(bot, step.QuestId, state.CurrentTargetGuid);
                    if (bot->GetQuestStatus(step.QuestId) != QUEST_STATUS_NONE)
                    {
                        AdvanceToNextStep(state);
                    }

                    break;
                }

                case StepPhase::Looting:
                    break; // unreachable for this step type
            }
        }

        // Walk to a quest giver and turn in a real, already-complete
        // quest, fully automatically. Same interaction-range discipline
        // as TickAcceptQuest.
        void TickTurnInQuest(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            switch (state.CurrentPhase)
            {
                case StepPhase::Approaching:
                {
                    if (state.CurrentTargetGuid.IsEmpty())
                    {
                        Creature* giver = bot->FindNearestCreature(step.CreatureEntry, step.SearchRadius, true);
                        if (!giver)
                        {
                            return;
                        }

                        state.CurrentTargetGuid = giver->GetGUID();
                        Navigation::MoveTo(bot, giver->GetPositionX(), giver->GetPositionY(), giver->GetPositionZ());
                        return;
                    }

                    Creature* giver = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!giver)
                    {
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        return;
                    }

                    if (bot->GetDistance(giver) <= InteractionToleranceYards)
                    {
                        state.CurrentPhase = StepPhase::Acting;
                    }

                    break;
                }

                case StepPhase::Acting:
                {
                    QuestEngine::RequestChooseReward(
                        bot, step.QuestId, state.CurrentTargetGuid, step.RewardChoiceIndex);
                    if (bot->IsQuestRewarded(step.QuestId))
                    {
                        AdvanceToNextStep(state);
                    }

                    break;
                }

                case StepPhase::Looting:
                    break; // unreachable for this step type
            }
        }
    } // namespace

    void Tick(Player* bot, BotGuideState& state)
    {
        if (!bot || state.Finished)
        {
            return;
        }

        if (state.CurrentStep >= state.Steps.size())
        {
            state.Finished = true;
            return;
        }

        GuideStep const& step = state.Steps[state.CurrentStep];

        switch (step.Type)
        {
            case StepType::MoveTo:
                TickMoveTo(bot, step, state);
                break;

            case StepType::KillNearest:
                TickKillNearest(bot, step, state);
                break;

            case StepType::AcceptQuest:
                TickAcceptQuest(bot, step, state);
                break;

            case StepType::TurnInQuest:
                TickTurnInQuest(bot, step, state);
                break;
        }
    }
} // namespace AutonomousPlayer::GuideRuntime
