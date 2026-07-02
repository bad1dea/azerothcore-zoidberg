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
#include "Combat/CombatExecutor.h"
#include "Combat/CombatIntent.h"
#include "Creature.h"
#include "EncounterModel/BotEncounterModel.h"
#include "Inventory/BotLoot.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "Pets/BotPets.h"
#include "Player.h"
#include "QuestEngine/BotQuestEngine.h"
#include "Recovery/PetAcquisitionPolicy.h"
#include "Recovery/PetRecoveryPolicy.h"

#include <algorithm>
#include <limits>
#include <list>
#include <optional>

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
            state.OperationTicks = 0;
        }

        // Shared bounded-wait check (ADR-028): any step/phase that would
        // otherwise wait indefinitely calls this once per tick. Returns
        // true once the bound is exceeded, having already marked the
        // guide `Failed`/`Finished` -- caller should stop processing this
        // tick immediately in that case. This directly closes the
        // external review's "several infinite waits remain" finding for
        // every operation it names except `KillNearest`'s `Approaching`
        // (already bounded by `MaxApproachTicks`, ADR-023) and loot
        // success verification (separate, smaller concern, not a wait).
        // `bot` is used only to stop movement on the timeout path (see
        // below) -- the timeout bookkeeping itself only needs `state`.
        bool OperationTimedOut(Player* bot, BotGuideState& state)
        {
            if (++state.OperationTicks > MaxOperationTicks)
            {
                state.Failed = true;
                state.Finished = true;

                // ADR-035, closes KNOWN_FAILURES.md #10: found live --
                // a `Navigation::MoveTo`/`Combat::RequestAttack` order
                // issued earlier in this step keeps physically driving
                // the character's `MotionMaster` even after the guide's
                // own bookkeeping gives up here. A real bot walked
                // itself off reachable terrain and died, unattended,
                // chasing an already-abandoned `guidestartmoveto` target
                // well past this function's own `Failed=true`. Stopping
                // movement explicitly on every bail-out path -- not just
                // marking the guide's state -- closes that gap: `bot`
                // stops exactly where it is (`Unit::StopMoving()`, a
                // standard public engine call, not a position write) and
                // waits for the next real command instead of continuing
                // to execute a decision this module itself has already
                // abandoned.
                if (bot)
                {
                    bot->StopMoving();
                }

                return true;
            }

            return false;
        }

        // Target selection safety (ADR-031, external review point 3):
        // whether `candidate` is a legitimate objective for `bot` to pull
        // right now, using only real, authoritative engine state -- no
        // heuristics. A real human player would never (or could never)
        // attack a friendly NPC, a creature already evading a prior pull,
        // one another player has already tapped or is actively fighting,
        // or one they cannot actually see. Every one of these was
        // previously unchecked: `FindNearestNonBlacklisted` only filtered
        // dead/blacklisted candidates, so `KillNearest` could select (and
        // then either uselessly attack-request-loop against, or worse,
        // steal a kill from) any of these.
        bool IsSafeToEngage(Player* bot, Creature* candidate)
        {
            if (candidate->IsInEvadeMode())
            {
                // A creature actively resetting from an earlier pull
                // (its own or someone else's) is not a legitimate target
                // -- attacking it now would either no-op or produce a
                // confusing half-reset fight.
                return false;
            }

            if (!bot->IsValidAttackTarget(candidate))
            {
                // Deliberately `IsValidAttackTarget`, NOT `IsHostileTo`
                // -- found live, not by inspection: most low-level
                // questing wildlife (Mottled Boar confirmed live) is
                // faction-neutral, not Hostile, yet is a completely
                // legitimate kill target. A first version of this check
                // used `IsHostileTo` and would have permanently rejected
                // Mottled Boars -- the exact target this whole project's
                // combat testing has relied on -- a real regression
                // caught before ever reaching KillNearest.
                // `IsValidAttackTarget` is the engine's own real
                // attackability check (reputation/faction rank, immunity
                // flags, dead/unselectable state), which correctly
                // treats attackable-neutral creatures as legitimate while
                // still excluding actually-friendly NPCs (vendors,
                // questgivers, guards).
                return false;
            }

            if (candidate->hasLootRecipient() && !candidate->isTappedBy(bot))
            {
                // Someone else (or their group) already has kill/loot
                // rights on this creature -- attacking it would be
                // kill-stealing, not a real solo pull, and the bot would
                // get no credit/loot for the kill regardless.
                return false;
            }

            for (Unit* attacker : candidate->getAttackers())
            {
                if (attacker && attacker->IsPlayer() && attacker != bot)
                {
                    // Another player is already actively fighting this
                    // creature -- even before tap registers (tap is set
                    // on first damage dealt, not on aggro), engaging the
                    // same target now is still kill-stealing/interference
                    // a real player would avoid.
                    return false;
                }
            }

            if (!bot->IsWithinLOSInMap(candidate))
            {
                // No real line of sight -- a real player cannot target
                // what they cannot see, and combat opcodes issued against
                // an unreachable-by-sight target would just stall like
                // any other unreachable target (KNOWN_FAILURES.md #3),
                // except this catches it at selection time instead of
                // burning a full MaxApproachTicks timeout first.
                return false;
            }

            return true;
        }

        // Nearest live, safe-to-engage creature of `entry` within
        // `range`, excluding any guid in `blacklist` -- Player::
        // FindNearestCreature has no exclusion parameter, so this
        // enumerates candidates directly (same
        // GetCreatureListWithEntryInGrid primitive the
        // .autonomousplayer multipull debug command already uses) and
        // picks the nearest non-blacklisted, safe one manually.
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

                if (!IsSafeToEngage(bot, candidate))
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

        // Keep a *live* pet defensive and command it onto the engagement
        // planner's selected target explicitly. REACT_AGGRESSIVE would let
        // the pet acquire unrelated nearby creatures and violates the
        // conservative one-planned-target/zero-desired-adds pull policy.
        // Dead/missing/dismissed pets are NOT this function's concern
        // (ADR-039) -- `GuideRuntime::Tick` checks `Recovery::PlanPetRecovery`
        // once, centrally, before any step (including this one) ever
        // runs, so by the time `EnsurePetAssists` is reached the pet is
        // either genuinely alive or genuinely absent; either way there is
        // nothing productive to do here beyond the alive case.
        void EnsurePetAssists(Player* bot, ObjectGuid const& targetGuid)
        {
            Pets::PetSnapshot snapshot = Pets::BuildSnapshot(bot);
            if (!snapshot.HasPet || !snapshot.Alive)
            {
                return;
            }

            if (snapshot.React != REACT_DEFENSIVE)
            {
                Pets::RequestSetPetReactState(bot, REACT_DEFENSIVE);
            }

            if (!targetGuid.IsEmpty() && snapshot.VictimGuid != targetGuid)
            {
                Combat::Execute(bot, Combat::CombatIntent{ Combat::IntentKind::AssistPetOnTarget, targetGuid, 0 });
            }
        }

        void TickMoveTo(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            if (OperationTimedOut(bot, state))
            {
                // Bounded (ADR-028): a one-shot MoveTo that never arrives
                // (unreachable point, stuck navmesh) previously waited
                // forever -- now the whole guide stops with `Failed=true`
                // rather than sitting frozen indefinitely.
                return;
            }

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
        // order) is deliberate, hard-won knowledge, not a stylistic
        // choice -- see KNOWN_FAILURES.md #3 for the full story: two
        // earlier attempts (gate on arrival before attacking; call a bare
        // `MotionMaster::MoveChase` with no attack request at all) were
        // each live-tested and each produced a permanent stall. Real
        // diagnostics proved a bare `MoveChase` produces *zero* bot
        // movement -- `Unit::Attack()` (via `HandleAttackSwingOpcode`,
        // inside `RequestAttack`) called *before* its own `MoveChase` is
        // required for the chase to actually engage. `RequestAttack` is
        // idempotent to re-issue every tick while approaching.
        //
        // Confirmation checks `bot->GetVictim() == target` -- the bot's
        // own real current attack target -- NOT `bot->IsInCombat()`.
        // `IsInCombat()` only means "something is fighting me," which an
        // unrelated add attacking the bot mid-approach would also
        // satisfy, causing a false-positive transition to `Engaged` while
        // the actual objective target was never touched (found via
        // external review, not live testing -- a real correctness gap
        // this component's own test scenarios never happened to trigger,
        // since none of them had a second hostile creature aggro during
        // approach).
        //
        // `Approaching` is bounded by `MaxApproachTicks`. A target that
        // never confirms engagement in time is blacklisted (scoped to
        // this guide step, cleared on `AdvanceToNextStep`) and a
        // different candidate is selected -- closing the "retry forever"
        // gap the research document's failure-handling section calls
        // out, instead of leaving it implicit.
        //
        // EncounterModel now actually gates the Approaching -> Engaged
        // transition (first real behavior consumer of it, per the
        // research document's step 2 being diagnostics *before*
        // decisions -- this is the decision): even once `GetVictim()`
        // confirms the bot is attacking its own objective target, the
        // transition is withheld while `HasUnplannedAdd()` is true. This
        // is a deliberately conservative default matching the research
        // document's "one desired target and zero desired adds" leveling
        // policy -- a messy multi-target encounter should not be silently
        // treated as a clean single pull. If the add situation doesn't
        // clear before `MaxApproachTicks`, the objective target still
        // gets blacklisted and retargeted like any other timeout, rather
        // than the guide getting stuck waiting on the add specifically.
        void TickKillNearest(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            switch (state.CurrentPullState)
            {
                case PullState::Selecting:
                {
                    if (OperationTimedOut(bot, state))
                    {
                        // Bounded (ADR-028): previously, if nothing
                        // matched (or everything got blacklisted) this
                        // retried forever with no way out. Now the whole
                        // guide stops with `Failed=true` rather than
                        // spinning indefinitely.
                        return;
                    }

                    Creature* target = FindNearestNonBlacklisted(
                        bot, step.CreatureEntry, step.SearchRadius, state.BlacklistedTargets);
                    if (!target)
                    {
                        // Nothing available (or everything found so far
                        // is blacklisted) -- retry next tick.
                        return;
                    }

                    // Note: `OperationTicks` is deliberately NOT reset
                    // here -- it bounds the whole KillNearest step (only
                    // reset on `AdvanceToNextStep`), so a pathological
                    // cycle of targets dying right as they're found still
                    // trips the bound eventually instead of resetting the
                    // clock every time.
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

                    if (bot->GetVictim() != target && !IsSafeToEngage(bot, target))
                    {
                        // Re-checked every tick, not just at selection
                        // time (ADR-031): something changed while we were
                        // still walking over -- most plausibly another
                        // player tapped/engaged it first, or it started
                        // evading. Only abandons before the bot has
                        // actually committed (`GetVictim() != target`) --
                        // once genuinely attacking, a real player
                        // wouldn't stop mid-swing over a status change.
                        state.BlacklistedTargets.push_back(state.CurrentTargetGuid);
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

                    Combat::Execute(bot,
                        Combat::CombatIntent{ Combat::IntentKind::EngageTarget, state.CurrentTargetGuid, 0 });
                    EnsurePetAssists(bot, state.CurrentTargetGuid);

                    if (bot->GetVictim() == target)
                    {
                        EncounterModel::Snapshot snapshot =
                            EncounterModel::BuildSnapshot(bot, state.CurrentTargetGuid);
                        if (!snapshot.HasUnplannedAdd())
                        {
                            state.CurrentPullState = PullState::Engaged;
                        }
                        // else: bot is genuinely attacking its own
                        // objective target, but something else is also
                        // attacking the bot -- stay in Approaching
                        // (RequestAttack keeps re-issuing, real damage
                        // keeps landing on the real target either way)
                        // rather than confirming a "clean" pull that
                        // isn't. Bounded by MaxApproachTicks above like
                        // any other Approaching stall.
                    }

                    break;
                }

                case PullState::Engaged:
                {
                    Creature* target = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!target || !target->IsAlive())
                    {
                        state.CurrentPullState = PullState::Looting;
                        break;
                    }

                    EnsurePetAssists(bot, state.CurrentTargetGuid);

                    if (OperationTimedOut(bot, state))
                    {
                        // Bounded (ADR-028): previously, once "Engaged"
                        // there was no deadline at all -- a target that
                        // evaded, reset, or simply never died (e.g. the
                        // bot's own damage output too low, a scripted
                        // unkillable creature) would wait forever. Now
                        // the whole guide stops with `Failed=true`
                        // instead. This does not distinguish evade from
                        // "just a slow kill" -- a real evade-specific
                        // signal is separate, later scope.
                        return;
                    }

                    // ADR-029: the smallest possible "class controller"
                    // slice. Real resource/cooldown/range requirements
                    // (ADR-018) apply for real -- a failed attempt (e.g.
                    // not enough rage yet) is a harmless, expected no-op;
                    // RequestAttack's melee swing keeps landing
                    // regardless, this is opportunistic bonus damage, not
                    // the only source of damage.
                    if (step.OpportunisticSpellId != 0)
                    {
                        Combat::Execute(bot,
                            Combat::CombatIntent{
                                Combat::IntentKind::UseAbility, state.CurrentTargetGuid, step.OpportunisticSpellId });
                    }

                    break;
                }

                case PullState::Looting:
                {
                    // Real loot verification (ADR-030): `Inventory::LootCorpse`'s
                    // own doc comment already says to verify results via
                    // the corpse's actual state afterward, not just trust
                    // its bool return (which only means "a loot session
                    // was opened and released," not "everything was
                    // actually taken"). Compare `corpse->loot` before and
                    // after: verified if nothing lootable was left
                    // (empty items, zero gold) once the session closes.
                    if (Creature* corpse = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid))
                    {
                        state.LastLootAttempted = true;

                        Inventory::LootCorpse(bot, corpse);

                        state.LastLootVerified = corpse->loot.items.empty() && corpse->loot.gold == 0;
                    }
                    else
                    {
                        // Corpse despawned before we could loot it --
                        // real, honest failure, not silently ignored.
                        state.LastLootAttempted = false;
                        state.LastLootVerified = false;
                    }

                    // Best-effort: this slice records whether looting was
                    // verified (visible via .autonomousplayer guidestatus)
                    // but does not retry a missed loot window -- a corpse
                    // that fails to fully loot (rare; would indicate a
                    // real bug elsewhere, since a normal single-item drop
                    // should always be fully autostored) still lets the
                    // guide continue rather than getting stuck over loot.
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
            if (OperationTimedOut(bot, state))
            {
                // Bounded (ADR-028): previously neither the
                // search-and-walk-to-questgiver wait nor the accept-
                // request retry wait had any deadline -- an unreachable
                // questgiver or a request that never resolves
                // (prerequisite not met, quest log full, etc.) would
                // retry forever. Now the whole guide stops with
                // `Failed=true` instead.
                return;
            }

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
            if (OperationTimedOut(bot, state))
            {
                // Bounded (ADR-028): same reasoning as TickAcceptQuest --
                // neither the search-and-walk wait nor the turn-in
                // request retry wait previously had a deadline.
                return;
            }

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

        // Pet recovery (ADR-039), checked BEFORE the current step gets a
        // chance to run at all -- the Singular "Recover" stage applied to
        // pet maintenance. `CurrentStep`/`CurrentPhase`/`CurrentPullState`
        // etc. are deliberately left completely untouched here: skipping
        // the step dispatch this tick is the entire mechanism for
        // "pausing" the guide, and simply not skipping it once
        // `PlanPetRecovery` next returns `nullopt` (pet alive again, or
        // recovery genuinely not applicable) is the entire mechanism for
        // "resuming" it -- no separate save/restore of guide state is
        // needed because none of it was ever mutated while paused.
        //
        // Gated on `CurrentTargetGuid.IsEmpty()` (ADR-040 fix, found live):
        // an earlier version ran this check unconditionally every tick,
        // including while a real objective target was actively being
        // pursued (`KillNearest`'s `Approaching`/`Engaged`, or a quest
        // giver interaction). `PlanPetRecovery` only requires
        // `!bot->IsInCombat()`, which is not a perfectly stable signal
        // moment-to-moment (a real evade, or a brief gap before the first
        // hit registers, both read as "not in combat" while a target is
        // still very much a live, in-progress objective) -- when it fired
        // during one of those windows, `Tick` returning early starved
        // `OperationTimedOut`'s own bounded-wait counter of ticks (it's
        // only incremented inside the step dispatch this skips), which
        // doesn't break correctness (the guide still eventually hits its
        // own tick-based bound and fails cleanly) but measurably stretches
        // wall-clock time to do so -- confirmed live via
        // `live_regression_suite.py` starting to intermittently exceed its
        // wall-clock timeout after this recovery check was added. An
        // empty `CurrentTargetGuid` is a precise proxy for "genuinely
        // between objectives, safe to pause for" across every step type
        // that uses it (`KillNearest`, quest accept/turn-in) -- a real
        // pursuit in progress is never preempted.
        if (state.CurrentTargetGuid.IsEmpty())
        {
            Pets::PetSnapshot petSnapshot = Pets::BuildSnapshot(bot);
            if (petSnapshot.HasPet)
            {
                state.LastKnownPetGuid = petSnapshot.Guid;
            }
            Pets::PetState petState = Pets::ClassifyPetState(bot, petSnapshot, state.LastKnownPetGuid);
            if (std::optional<Combat::CombatIntent> recovery = Recovery::PlanPetRecovery(bot, petState))
            {
                Combat::Execute(bot, *recovery);
                return;
            }

            // Pet acquisition (ADR-041), same gate, same
            // pause-by-skipping-dispatch mechanism, deliberately checked
            // second (a `Dismissed`/`Missing*` pet from `PlanPetRecovery`
            // above already means this branch's `PetState::NoPet`
            // precondition can't hold, so ordering is a documentation
            // choice, not a real race). Real, separate scope from
            // recovery: acquiring a first pet, not restoring an existing
            // one.
            if (std::optional<Combat::CombatIntent> acquisition = Recovery::PlanPetAcquisition(bot, petState))
            {
                Combat::Execute(bot, *acquisition);
                return;
            }
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
