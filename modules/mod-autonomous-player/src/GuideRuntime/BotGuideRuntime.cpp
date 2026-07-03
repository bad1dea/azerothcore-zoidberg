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
#include "Combat/CombatExecutor.h"
#include "Combat/CombatIntent.h"
#include "Creature.h"
#include "Economy/BotEconomy.h"
#include "EncounterModel/BotEncounterModel.h"
#include "Inventory/BotLoot.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pets/BotPets.h"
#include "Player.h"
#include "QuestEngine/BotQuestEngine.h"
#include "Recovery/PetAcquisitionPolicy.h"
#include "Recovery/PetRecoveryPolicy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

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
            state.KillsCompleted = 0;
            state.LastSelection = SelectionDiagnostics{};
            state.TurnInEngineRefused = false;
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
        bool OperationTimedOut(Player* bot, BotGuideState& state, uint32_t maxTicks = MaxOperationTicks)
        {
            if (++state.OperationTicks > maxTicks)
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
        //
        // `diag` (optional): per-rejection-reason counters
        // (KNOWN_FAILURES.md #21's diagnosability note) -- each
        // rejection increments exactly one counter, so a selection sweep
        // over many candidates ends with a breakdown of *why* a drought
        // is a drought instead of the three-causes-one-signature
        // ambiguity that cost a real session most of its diagnosis time.
        bool IsSafeToEngage(Player* bot, Creature* candidate, SelectionDiagnostics* diag = nullptr)
        {
            if (candidate->IsInEvadeMode())
            {
                // A creature actively resetting from an earlier pull
                // (its own or someone else's) is not a legitimate target
                // -- attacking it now would either no-op or produce a
                // confusing half-reset fight.
                if (diag)
                    ++diag->Evading;
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
                if (diag)
                    ++diag->NotAttackable;
                return false;
            }

            if (candidate->hasLootRecipient() && !candidate->isTappedBy(bot))
            {
                // Someone else (or their group) already has kill/loot
                // rights on this creature -- attacking it would be
                // kill-stealing, not a real solo pull, and the bot would
                // get no credit/loot for the kill regardless.
                if (diag)
                    ++diag->Tapped;
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
                    if (diag)
                        ++diag->OtherPlayerAttacking;
                    return false;
                }
            }

            if (!bot->IsWithinLOSInMap(candidate, VMAP::ModelIgnoreFlags::M2))
            {
                // No real line of sight -- a real player cannot target
                // what they cannot see, and combat opcodes issued against
                // an unreachable-by-sight target would just stall like
                // any other unreachable target (KNOWN_FAILURES.md #3),
                // except this catches it at selection time instead of
                // burning a full MaxApproachTicks timeout first.
                //
                // `ModelIgnoreFlags::M2` matters and was found live, not
                // by inspection (KNOWN_FAILURES.md #21): the engine's own
                // spell-cast LoS check (`Spell::CheckCast`,
                // `Spell.cpp`) ignores M2 doodad models -- trees,
                // crystals, decorative props -- and real melee/spells
                // work straight through them. The first version of this
                // check used the default strict flags (M2s block sight),
                // which on doodad-dense terrain (Sunstrider Isle,
                // confirmed live: every creature zone-wide reported
                // los=false while a real attack walked over and fought
                // one without issue) rejected every candidate a real
                // player could genuinely fight, starving target
                // selection entirely. WMO buildings/terrain still block
                // normally under M2-ignore, matching real gameplay.
                if (diag)
                    ++diag->NoLineOfSight;
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
        //
        // `diag` is reset and refilled on every sweep, so it always
        // describes the latest tick's candidate reality (see
        // `SelectionDiagnostics`).
        Creature* FindNearestNonBlacklisted(
            Player* bot, uint32_t entry, float range, std::vector<ObjectGuid> const& blacklist,
            SelectionDiagnostics& diag)
        {
            std::list<Creature*> candidates;
            bot->GetCreatureListWithEntryInGrid(candidates, entry, range);

            diag = SelectionDiagnostics{};

            Creature* best = nullptr;
            float bestDistance = std::numeric_limits<float>::max();

            for (Creature* candidate : candidates)
            {
                ++diag.Candidates;

                if (!candidate->IsAlive())
                {
                    ++diag.Dead;
                    continue;
                }

                if (std::find(blacklist.begin(), blacklist.end(), candidate->GetGUID()) != blacklist.end())
                {
                    ++diag.Blacklisted;
                    continue;
                }

                if (!IsSafeToEngage(bot, candidate, &diag))
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

        // Ranged-pull mode decision (ADR-044): a step whose opportunistic
        // ability is genuinely ranged (real `SpellInfo` max range at or
        // above `RangedPullMinimumMaxRangeYards`) engages from range and
        // holds there instead of closing to melee -- the Gate 3 "ranged
        // pulls as a distinct behavior" bar. Reads the real DBC-backed
        // spell data, not a hardcoded per-spell list, so a future ranged
        // caster archetype gets the same behavior with no new code
        // (exactly how ADR-034's melee/ranged opportunistic composition
        // already generalized). A missing SpellInfo (bogus id) simply
        // means melee -- the ordinary engage is always the safe default.
        bool ShouldEngageRanged(GuideStep const& step)
        {
            if (step.OpportunisticSpellId == 0)
            {
                return false;
            }

            SpellInfo const* info = sSpellMgr->GetSpellInfo(step.OpportunisticSpellId);
            return info && info->GetMaxRange(false) >= Combat::RangedPullMinimumMaxRangeYards;
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

            // 2D deliberately (#14 follow-up, found the same hour the
            // grounded-movement fix deployed): authored waypoint Zs are
            // approximate (spawn-table averages, hand-read map points),
            // and the navmesh ground the bot now actually stands on can
            // differ by several yards. The old airborne movement masked
            // this -- the straight-line spline ended AT the requested Z,
            // mid-air, and a 3D check read distance 0. Live evidence:
            // Nelftestbot at the target's exact X/Y, Z 6.1yd below the
            // requested value, bound-failing while standing on the spot.
            if (bot->GetDistance2d(step.X, step.Y) <= ArrivalToleranceYards)
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
            // Per-cycle budget (ADR-048): a repeat-until-quest-complete
            // step resets its ticks after every completed kill+loot
            // cycle, so its budget bounds ONE cycle, not the whole
            // grind -- and a single legitimate cycle can exceed the
            // plain budget: found live, a level-1 Troll vs a level-2
            // boar was mid-fight, winning, when 46 ticks expired. 3x
            // (~60 real seconds) comfortably bounds any one honest
            // cycle while still catching a genuinely stuck one.
            uint32_t const cycleBudget =
                (step.RepeatUntilQuestComplete || step.RepeatKillCount != 0)
                    ? MaxOperationTicks * 3
                    : MaxOperationTicks;

            switch (state.CurrentPullState)
            {
                case PullState::Selecting:
                {
                    // Gate on ENTRY too, not only after a completed
                    // kill+loot cycle (the check further down): a
                    // re-issued route whose quest is already COMPLETE
                    // must skip the grind entirely, not owe one more
                    // kill first. Found live (ADR-048 follow-up): a
                    // grind that bound-failed at the turn-in step could
                    // not be resumed with a waypoint near the giver,
                    // because the giver's surroundings had no grind
                    // targets and the step spun Selecting to its bound
                    // despite the objectives being done.
                    if (step.RepeatUntilQuestComplete && step.QuestId != 0
                        && bot->GetQuestStatus(step.QuestId) != QUEST_STATUS_INCOMPLETE)
                    {
                        AdvanceToNextStep(state);
                        break;
                    }

                    if (OperationTimedOut(bot, state, cycleBudget))
                    {
                        // Bounded (ADR-028): previously, if nothing
                        // matched (or everything got blacklisted) this
                        // retried forever with no way out. Now the whole
                        // guide stops with `Failed=true` rather than
                        // spinning indefinitely.
                        return;
                    }

                    Creature* target = FindNearestNonBlacklisted(
                        bot, step.CreatureEntry, step.SearchRadius, state.BlacklistedTargets,
                        state.LastSelection);
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

                    // Defend during approach (ADR-050, the Singular
                    // model's "defend yourself" rule): while still
                    // WALKING to the planned target, anything already
                    // beating on the bot is the real fight. Observed
                    // live on the 1->12 run (first organic
                    // hasUnplannedAdd, closing KNOWN_FAILURES.md #20's
                    // residual): approach paths through a dense camp
                    // collect melee attackers the bot never swung back
                    // at -- 240->127 health across one 40-second
                    // approach, repeated to death. Retarget to the
                    // nearest live melee-range attacker instead; kill
                    // credit is secondary to surviving, and in
                    // practice camp attackers are the objective entry
                    // anyway. Only while not yet committed
                    // (GetVictim() != target), mirroring the
                    // abandon-before-commit rule below.
                    if (bot->GetVictim() != target)
                    {
                        Unit* adjacentAttacker = nullptr;
                        float best = 10.0f;
                        for (Unit* attacker : bot->getAttackers())
                        {
                            if (!attacker || !attacker->IsAlive() || !attacker->IsCreature())
                            {
                                continue;
                            }
                            float dist = bot->GetDistance(attacker);
                            if (dist < best && bot->IsValidAttackTarget(attacker))
                            {
                                best = dist;
                                adjacentAttacker = attacker;
                            }
                        }
                        if (adjacentAttacker
                            && adjacentAttacker->GetGUID() != state.CurrentTargetGuid)
                        {
                            state.CurrentTargetGuid = adjacentAttacker->GetGUID();
                            state.ApproachTicks = 0;
                            target = adjacentAttacker->ToCreature();
                        }
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

                    if (ShouldEngageRanged(step))
                    {
                        // ADR-044: hold at the ability's real range and
                        // open from there instead of walking into melee
                        // contact. Same per-tick re-issue pattern as the
                        // melee engage; same `GetVictim()` confirmation
                        // below (`Unit::Attack(target, false)` sets the
                        // victim exactly like the melee opcode path).
                        Combat::Execute(bot,
                            Combat::CombatIntent{ Combat::IntentKind::EngageTargetRanged,
                                state.CurrentTargetGuid, step.OpportunisticSpellId });
                    }
                    else
                    {
                        Combat::Execute(bot,
                            Combat::CombatIntent{ Combat::IntentKind::EngageTarget, state.CurrentTargetGuid, 0 });
                    }
                    EnsurePetAssists(bot, state.CurrentTargetGuid);
                    Combat::MaintainFacing(bot);

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
                    Combat::MaintainFacing(bot);

                    if (OperationTimedOut(bot, state, cycleBudget))
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

                    // Melee fallback for a ranged engagement (ADR-044):
                    // a leveling mob runs to its attacker, and inside the
                    // opener's real minimum range the per-shot CheckCast
                    // just skips shots -- without this the bot would
                    // stand there taking hits doing nothing, which no
                    // real player would. Once the target is genuinely at
                    // melee reach, commit to the ordinary melee engage
                    // (swings on, contact chase); Auto Shot's autorepeat
                    // stays armed and resumes by itself if the target
                    // ever flees back out (real engine behavior,
                    // `Unit::_UpdateAutoRepeatSpell`).
                    if (ShouldEngageRanged(step) && bot->IsWithinMeleeRange(target))
                    {
                        Combat::Execute(bot,
                            Combat::CombatIntent{ Combat::IntentKind::EngageTarget, state.CurrentTargetGuid, 0 });
                    }

                    // ADR-029: the smallest possible "class controller"
                    // slice. Real resource/cooldown/range requirements
                    // (ADR-018) apply for real -- a failed attempt (e.g.
                    // not enough rage yet) is a harmless, expected no-op;
                    // RequestAttack's melee swing keeps landing
                    // regardless, this is opportunistic bonus damage, not
                    // the only source of damage.
                    //
                    // The `IsNonMeleeSpellCast` guard exists because a
                    // cast-time opener otherwise self-interrupts forever
                    // -- an earlier comment here predicted that and it
                    // was then observed live exactly as written
                    // (Warlock Shadow Bolt, `KNOWN_FAILURES.md` #25: the
                    // per-tick re-cast cancelled every in-flight bolt,
                    // zero ever landed, the bot stood taking melee hits
                    // until the ADR-028 bound fired). `skipAutorepeat=
                    // true` keeps the Auto Shot archetype's behavior
                    // exactly as before: an armed autorepeat doesn't
                    // count as "casting" (re-casting 75 is the same
                    // harmless no-op it always was), only a genuine
                    // in-flight cast blocks the re-issue.
                    if (step.OpportunisticSpellId != 0
                        && !bot->IsNonMeleeSpellCast(false, false, true))
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

                        // `Loot::isLooted()` is the engine's own
                        // "nothing left to take" predicate (gold gone,
                        // `unlootedCount` zero -- looted items are
                        // FLAGGED, never erased from `loot.items`, so
                        // the previous `items.empty()` check here could
                        // literally never report true for any corpse
                        // that dropped an item at all, fully looted or
                        // not; found while root-causing KNOWN_FAILURES.md
                        // #26). It also counts per-player quest drops,
                        // so a left-behind quest item now correctly
                        // reads as unverified.
                        state.LastLootVerified = corpse->loot.isLooted();
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

                    // Repeat-until-quest-complete (ADR-048): the real
                    // "kill/collect until the objectives are done"
                    // semantic -- collection quests fill up through the
                    // loot autostore above, kill quests through ordinary
                    // kill credit, and the engine itself flips the
                    // quest's status from INCOMPLETE to COMPLETE the
                    // moment the last objective lands
                    // (`ItemAddedQuestCheck`/`KilledMonsterCredit`), so
                    // "still INCOMPLETE" is the exact keep-grinding
                    // predicate. Deliberately NOT `!CanCompleteQuest()`
                    // -- found live on this feature's very first full
                    // run (`KNOWN_FAILURES.md` #27): that helper only
                    // evaluates objectives while the status is still
                    // INCOMPLETE and returns false for an
                    // already-COMPLETE quest, so the first version of
                    // this gate kept grinding forever AFTER the quest
                    // completed (7/7 meat collected, bot hunting its
                    // 8th plainstrider) until the ADR-028 bound killed
                    // the step. Resetting the per-cycle bookkeeping is
                    // a deliberate, documented ADR-028 exception (see
                    // the GuideStep field's comment): a verified
                    // kill+loot cycle is real progress, and the bound
                    // still limits each individual cycle. The per-step
                    // blacklist is deliberately KEPT across cycles -- a
                    // target blacklisted as unreachable stays
                    // unreachable.
                    if (step.RepeatUntilQuestComplete && step.QuestId != 0
                        && bot->GetQuestStatus(step.QuestId) == QUEST_STATUS_INCOMPLETE)
                    {
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        state.CurrentPullState = PullState::Selecting;
                        state.ApproachTicks = 0;
                        state.OperationTicks = 0;
                        break;
                    }

                    // Repeat-count grinding (ADR-051): same chained
                    // kill+loot loop as ADR-048, but bounded by a plain
                    // cycle count instead of a quest -- pure XP grinding
                    // between quests previously paid one full external
                    // command round-trip per single kill, which on the
                    // live 1->12 run cost more wall-clock than the
                    // fights themselves. Same deliberate per-cycle
                    // bookkeeping reset (a verified kill+loot cycle IS
                    // progress); same kept blacklist.
                    if (step.RepeatKillCount != 0
                        && ++state.KillsCompleted < step.RepeatKillCount)
                    {
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        state.CurrentPullState = PullState::Selecting;
                        state.ApproachTicks = 0;
                        state.OperationTicks = 0;
                        break;
                    }

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
            // Idempotent (ADR-048): a quest already in the log (or
            // already complete/rewarded) means this step's work is
            // already done -- found live when re-running a grind route
            // after a bounded failure: the re-accept request is
            // rejected by the engine for real, so without this the
            // whole route bounded out at step 0 and the route could
            // never be resumed. Skipping ahead is what a real player
            // re-following a guide does too.
            if (bot->GetQuestStatus(step.QuestId) != QUEST_STATUS_NONE)
            {
                AdvanceToNextStep(state);
                return;
            }

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
            // Idempotent, mirror of TickAcceptQuest's check (ADR-048):
            // an already-rewarded quest has nothing left to turn in --
            // makes a route re-run after a mid-route bounded failure
            // resume cleanly instead of failing here.
            if (bot->GetQuestRewardStatus(step.QuestId))
            {
                AdvanceToNextStep(state);
                return;
            }

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
                        break;
                    }

                    // #29 diagnosability: the engine only ever reports a
                    // turn-in refusal (full bags for a choice-reward item,
                    // most commonly) to the headless client session, so a
                    // wedged turn-in used to read as a generic timeout in
                    // `guidestatus`. Ask the same predicate the opcode
                    // handler uses and surface it.
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(step.QuestId))
                    {
                        state.TurnInEngineRefused =
                            !bot->CanRewardQuest(quest, step.RewardChoiceIndex, false);
                    }

                    break;
                }

                case StepPhase::Looting:
                    break; // unreachable for this step type
            }
        }

        // KNOWN_FAILURES.md #29's durable fix, first slice: walk to the
        // nearest `CreatureEntry` vendor and sell every gray item.
        // Same Approaching/Acting shape as TickTurnInQuest; completion
        // is a re-count reading zero (real inventory state, ADR-030's
        // verify-don't-assume contract), checked on entry so the step
        // is idempotent -- a bot with no grays advances immediately and
        // any route containing this step stays re-issuable.
        void TickSellJunk(Player* bot, GuideStep const& step, BotGuideState& state)
        {
            if (Economy::CountSellableGrayItems(bot) == 0)
            {
                AdvanceToNextStep(state);
                return;
            }

            if (OperationTimedOut(bot, state))
            {
                // Bounded (ADR-028): covers both the vendor
                // search-and-walk wait and a sell submission that never
                // empties the grays (e.g. a vendor flagged
                // CREATURE_FLAG_EXTRA_NO_SELL_VENDOR refuses every item
                // -- only ever reported to the headless session).
                return;
            }

            switch (state.CurrentPhase)
            {
                case StepPhase::Approaching:
                {
                    if (state.CurrentTargetGuid.IsEmpty())
                    {
                        Creature* vendor = bot->FindNearestCreature(step.CreatureEntry, step.SearchRadius, true);
                        if (!vendor)
                        {
                            return;
                        }

                        state.CurrentTargetGuid = vendor->GetGUID();
                        Navigation::MoveTo(bot, vendor->GetPositionX(), vendor->GetPositionY(), vendor->GetPositionZ());
                        return;
                    }

                    Creature* vendor = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!vendor)
                    {
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        return;
                    }

                    if (bot->GetDistance(vendor) <= InteractionToleranceYards)
                    {
                        state.CurrentPhase = StepPhase::Acting;
                    }

                    break;
                }

                case StepPhase::Acting:
                {
                    Creature* vendor = ObjectAccessor::GetCreature(*bot, state.CurrentTargetGuid);
                    if (!vendor)
                    {
                        state.CurrentTargetGuid = ObjectGuid::Empty;
                        state.CurrentPhase = StepPhase::Approaching;
                        return;
                    }

                    Economy::SellGrayItems(bot, vendor);
                    // Completion is the re-count on the next tick's
                    // entry check, not this call returning.
                    break;
                }

                case StepPhase::Looting:
                    break; // unreachable for this step type
            }
        }
    } // namespace

    bool TickAmbient(Player* bot, BotGuideState& state)
    {
        // Background bot maintenance -- pet recovery/acquisition
        // currently -- that should happen regardless of whether a guide
        // is actively running (ADR-042, closing `KNOWN_FAILURES.md`
        // #16's real gap: this logic used to live entirely inside
        // `Tick()`, which `BotLifecycleMgr::Update` only calls while
        // `!state.Finished` -- a fully idle bot between guides, or one
        // that was never given a guide at all, got zero pet maintenance
        // no matter how long it sat there). Deliberately a *separate*
        // function from `Tick()`, called unconditionally by
        // `BotLifecycleMgr::Update` every tick interval for every
        // registered bot -- not folded back into `Tick()` with the
        // `state.Finished` check loosened, since that would conflate
        // "is a guide step allowed to run" with "should this bot's pet
        // be maintained," which are genuinely orthogonal concerns.
        //
        // Still gated on `state.CurrentTargetGuid.IsEmpty()` (ADR-040):
        // even though this function no longer lives inside `Tick()`'s own
        // step dispatch, the same real objective-in-progress signal still
        // applies whenever a guide *is* running -- a live `KillNearest`
        // pursuit or quest interaction must not be preempted by pet
        // maintenance mid-pursuit, same reasoning as ADR-040's original
        // fix, this just keeps it correct now that the call site moved.
        if (!bot || !state.CurrentTargetGuid.IsEmpty())
        {
            return false;
        }

        Pets::PetSnapshot petSnapshot = Pets::BuildSnapshot(bot);
        if (petSnapshot.HasPet)
        {
            state.LastKnownPetGuid = petSnapshot.Guid;
        }
        Pets::PetState petState = Pets::ClassifyPetState(bot, petSnapshot, state.LastKnownPetGuid);
        if (std::optional<Combat::CombatIntent> recovery = Recovery::PlanPetRecovery(bot, petState))
        {
            Combat::Execute(bot, *recovery);
            return true;
        }

        // Pet acquisition (ADR-041), same gate. Deliberately checked
        // second (a `Dismissed`/`Missing*` pet from `PlanPetRecovery`
        // above already means this branch's `PetState::NoPet`
        // precondition can't hold, so ordering is a documentation
        // choice, not a real race). Real, separate scope from recovery:
        // acquiring a first pet, not restoring an existing one.
        if (std::optional<Combat::CombatIntent> acquisition = Recovery::PlanPetAcquisition(bot, petState))
        {
            Combat::Execute(bot, *acquisition);
            return true;
        }

        // Warlock demon maintenance (ADR-047) -- mutually exclusive with
        // both Hunter policies above by class gate, so ordering is
        // again a documentation choice. Closes the live-confirmed gap
        // from the first Warlock probe: an imp existed entirely outside
        // the ambient maintenance envelope (stayed passive, would never
        // have been re-summoned after death/dismiss).
        if (std::optional<Combat::CombatIntent> demon = Recovery::PlanDemonMaintenance(bot, petState))
        {
            Combat::Execute(bot, *demon);
            return true;
        }

        return false;
    }

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

            case StepType::SellJunk:
                TickSellJunk(bot, step, state);
                break;
        }
    }
} // namespace AutonomousPlayer::GuideRuntime
