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
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "Player.h"

namespace AutonomousPlayer::GuideRuntime
{
    namespace
    {
        void AdvanceToNextStep(BotGuideState& state)
        {
            ++state.CurrentStep;
            state.ActionIssuedForCurrentStep = false;
            state.CurrentKillPhase = KillPhase::Approaching;
            state.CurrentKillTarget = ObjectGuid::Empty;
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
        // Navigation/Combat/Inventory primitives (no new opcode work).
        // Unlike MoveTo, this isn't a single fire-and-check action, so it
        // tracks its own sub-phase (KillPhase) and target (as a GUID,
        // resolved fresh every tick -- never a stored raw pointer, per
        // ADR-002).
        void TickKillNearest(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            switch (state.CurrentKillPhase)
            {
                case KillPhase::Approaching:
                {
                    if (state.CurrentKillTarget.IsEmpty())
                    {
                        Creature* target = bot->FindNearestCreature(step.CreatureEntry, step.SearchRadius, true);
                        if (!target)
                        {
                            // No live target found yet -- retry next tick.
                            return;
                        }

                        state.CurrentKillTarget = target->GetGUID();

                        // Same pattern as the .autonomousplayer attack
                        // debug command: issue a real walk-in plus a real
                        // attack request together (RequestAttack's own
                        // MoveChase handles closing the remaining
                        // distance and staying on the target).
                        Navigation::MoveTo(bot, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
                        Combat::RequestAttack(bot, state.CurrentKillTarget);
                        state.CurrentKillPhase = KillPhase::Attacking;
                    }

                    break;
                }

                case KillPhase::Attacking:
                {
                    Creature* target = ObjectAccessor::GetCreature(*bot, state.CurrentKillTarget);
                    if (!target || !target->IsAlive())
                    {
                        state.CurrentKillPhase = KillPhase::Looting;
                    }

                    break;
                }

                case KillPhase::Looting:
                {
                    if (Creature* corpse = ObjectAccessor::GetCreature(*bot, state.CurrentKillTarget))
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
        }
    }
} // namespace AutonomousPlayer::GuideRuntime
