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

        // Walk to + attack + loot the nearest creature of a given entry,
        // fully automatically -- composes the already-proven
        // Combat/Inventory primitives (no new opcode work). Unlike
        // MoveTo, this isn't a single fire-and-check action, so it tracks
        // its own sub-phase and target (as a GUID, resolved fresh every
        // tick -- never a stored raw pointer, per ADR-002).
        //
        // Waits for real arrival (MeleeEngageToleranceYards) before
        // issuing the attack request. Found live (twice) that a one-shot
        // Navigation::MoveTo snapshot toward the target's search-time
        // position is not enough: Mottled Boars have real wandering AI,
        // so a single fixed-point walk order can complete at a position
        // the target has since moved away from, permanently stranding the
        // bot out of range with nothing left to close the gap (see
        // KNOWN_FAILURES.md #3). Uses `MotionMaster::MoveChase` instead
        // -- the same real, continuous-follow production movement
        // generator `Combat::RequestAttack` itself uses once attacking
        // (ADR-012) -- so the bot keeps closing distance on a moving
        // target throughout the whole Approaching phase, not just once.
        void TickKillNearest(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            switch (state.CurrentPhase)
            {
                case StepPhase::Approaching:
                {
                    if (state.CurrentTargetGuid.IsEmpty())
                    {
                        Creature* target = bot->FindNearestCreature(step.CreatureEntry, step.SearchRadius, true);
                        if (!target)
                        {
                            // No live target found yet -- retry next tick.
                            return;
                        }

                        state.CurrentTargetGuid = target->GetGUID();
                        bot->GetMotionMaster()->MoveChase(target);
                        return;
                    }

                    Creature* target = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!target || !target->IsAlive())
                    {
                        // Died/despawned/unreachable before we arrived --
                        // give up on this guid and retry the search next
                        // tick rather than getting stuck.
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        return;
                    }

                    if (bot->GetDistance(target) <= MeleeEngageToleranceYards)
                    {
                        Combat::RequestAttack(bot, state.CurrentTargetGuid);
                        state.CurrentPhase = StepPhase::Acting;
                    }

                    break;
                }

                case StepPhase::Acting:
                {
                    Creature* target = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!target || !target->IsAlive())
                    {
                        state.CurrentPhase = StepPhase::Looting;
                    }

                    break;
                }

                case StepPhase::Looting:
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
