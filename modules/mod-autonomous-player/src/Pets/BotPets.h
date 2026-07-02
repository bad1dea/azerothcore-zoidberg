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

#ifndef AUTONOMOUS_PLAYER_BOT_PETS_H
#define AUTONOMOUS_PLAYER_BOT_PETS_H

#include "ObjectGuid.h"
#include "SharedDefines.h"
#include "Unit.h"
#include <cstdint>

class Creature;
class Player;

namespace AutonomousPlayer::Pets
{
    // Gate 3 pets design-pass first slice (ADR-037). Taming itself needed
    // zero new module code (ADR-036 found this live: `Combat::RequestCastSpell`
    // already casts Tame Beast correctly through the real engine
    // `EffectTameCreature`) -- what was actually missing was pet-state
    // *visibility* and a real, reusable primitive instead of a one-off
    // debug-command cast. This component is deliberately small: taming,
    // a status snapshot, and setting the pet to fight alongside its
    // owner. No pet ability rotation, no re-taming-on-death, no
    // beast-family-specific logic -- those are real, separate, later
    // slices if this one proves out.

    // The real Tame Beast spell id -- confirmed live (ADR-036): rejected
    // with a real, specific `SpellCastResult` when out of range or
    // moving (proving it's genuine engine-recognized spell data, not a
    // guess), accepted with `SPELL_CAST_OK` in range, and its real cast
    // time completing produces a genuine `character_pet` row and a live
    // `Pet` object. Deliberately not verified by name (this fork's own
    // `spell_dbc` mirror table is already known-incomplete/unreliable
    // for this, see KNOWN_FAILURES.md #9's investigation) -- verified by
    // observed real behavior instead.
    inline constexpr uint32_t TameBeastSpellId = 1515;

    // The real Revive Pet spell id (ADR-039). Confirmed live the same
    // empirical way as Tame Beast: casting it at a live pet returned a
    // real, specific `SpellCastResult`
    // (`SPELL_FAILED_ALREADY_HAVE_SUMMON`, not an unknown-spell error) --
    // consistent with this being implemented as a re-summon-style effect
    // that's correctly rejected while the pet is still alive (a live pet
    // counts as an active summon). Not yet confirmed against an actually
    // dead pet (see `RequestRevivePet`'s doc comment).
    inline constexpr uint32_t RevivePetSpellId = 982;

    // A real, standard WotLK Call Pet spell id -- **NOT yet empirically
    // confirmed live** (unlike `TameBeastSpellId`/`RevivePetSpellId`,
    // which were both directly cast-tested before being trusted). No
    // test Hunter reached the level to have this learned this session
    // (`Player::HasSpell` returned false for every candidate at the
    // levels reached) -- `RequestCallPet` and `PlanPetRecovery`'s
    // `MissingAlive` branch are gated on `HasSpell` for exactly this
    // reason, matching this project's established discipline of never
    // assuming a gated ability is available just because an id exists.
    // Whoever next levels a Hunter past this point should re-verify this
    // id the same way Tame Beast/Revive Pet were (cast it, read the real
    // `SpellCastResult`) before removing this caveat. Note also:
    // `SPELL_EFFECT_CALL_PET` (135) maps to `Spell::EffectNULL` in this
    // fork's generic effect table (confirmed in `SpellEffects.cpp`) --
    // but that does not necessarily mean this specific spell id is
    // broken, since `Player::SummonPet`'s own source has a direct
    // comment ("petentry == 0 for hunter 'call pet'") showing the real
    // Call Pet mechanic is expected to route through the same
    // `SummonPet(0, ...)` path `RequestRevivePet` already confirmed
    // works for a missing pet -- it may use a different, real effect
    // (e.g. `SPELL_EFFECT_SUMMON_PET`/`SPELL_EFFECT_RESURRECT_PET`) at
    // the DBC level rather than the generic 135. Unconfirmed either way.
    inline constexpr uint32_t CallPetSpellId = 883;

    // Immutable per-call snapshot of `bot`'s pet, if any (ADR-002
    // tick-safety: a plain value type, never stores a `Pet*` past the
    // call that built it). `HasPet=false` means every other field is
    // meaningless -- always check it first.
    struct PetSnapshot
    {
        bool HasPet = false;
        ObjectGuid Guid;
        uint32_t Entry = 0;
        bool Alive = false;
        uint32_t Health = 0;
        uint32_t MaxHealth = 0;
        ReactStates React = REACT_PASSIVE;

        // The pet's own current attack target, if any (ADR-037 followup):
        // added specifically to close the "does an aggressive pet
        // actually assist in combat" gap the first pets slice left open
        // -- a fast kill during manual polling couldn't distinguish
        // "the pet engaged" from "the bot alone killed it as usual."
        // Empty if the pet isn't currently attacking anything.
        ObjectGuid VictimGuid;
    };

    // Builds a snapshot of `bot->GetPet()` right now. Safe to call
    // whether or not a pet exists.
    [[nodiscard]] PetSnapshot BuildSnapshot(Player* bot);

    // Pet lifecycle state (ADR-039, corrected/expanded same day after a
    // real live-testing round -- see `KNOWN_FAILURES.md` #13's final
    // writeup). `Player::GetPet()` alone only tells you whether a *live*
    // pet object currently exists (`Active*`) -- it says nothing about a
    // pet that's real and recoverable but not currently loaded
    // (`Missing*`, confirmed live: the real engine effect behind Revive
    // Pet, `Spell::EffectResurrectPet`, explicitly handles this case by
    // reloading from `PetStable`/DB) versus one that's genuinely gone
    // (`Dismissed`) versus never having existed at all (`NoPet`).
    // Distinguishing `Missing*`'s Alive/Dead sub-state requires reading
    // the real stored health in `PetStable::GetUnslottedHunterPet()`,
    // not just presence -- that's why `ClassifyPetState` takes `bot`
    // directly (for that one synchronous read) rather than staying a
    // pure `(snapshot, guid)` function like the first version was.
    enum class PetState : uint8_t
    {
        NoPet,        // never tamed anything: HasPet=false, no stable entry, no last-known guid
        ActiveAlive,  // GetPet() resolves, IsAlive()
        ActiveDead,   // GetPet() resolves, !IsAlive()
        MissingAlive, // GetPet()==null, but a stable entry exists with stored Health > 0
        MissingDead,  // GetPet()==null, stable entry exists with stored Health == 0
        Dismissed,    // GetPet()==null, no stable entry, but a last-known guid was recorded
    };

    // `lastKnownPetGuid`: see `GuideRuntime::BotGuideState::LastKnownPetGuid`
    // -- the only piece of state this needs that the engine itself
    // doesn't track, used solely to tell `Dismissed` apart from `NoPet`
    // when there's no stable entry either.
    [[nodiscard]] PetState ClassifyPetState(Player* bot, PetSnapshot const& snapshot, ObjectGuid const& lastKnownPetGuid);

    // Detects a real, separate engine inconsistency found live while
    // investigating `KNOWN_FAILURES.md` #13 (it turned out NOT to be the
    // cause of that specific bug, but is real and worth keeping as its
    // own defensive check): `Player::GetPet()`'s own real source
    // resolves `Unit::GetPetGUID()` to a live object and returns null if
    // that resolution fails -- but it does NOT clear `GetPetGUID()`
    // itself when that happens (the engine's own source even has the
    // fix commented out: `//const_cast<Player*>(this)->SetPetGUID(0);`).
    // If this ever leaves a stale non-empty guid behind while `GetPet()`
    // is null, `EffectTameCreature` would silently refuse to tame a new
    // pet regardless of whether one is otherwise recoverable
    // (`if (m_caster->GetPetGUID()) return;`, confirmed by reading
    // `SpellEffects.cpp`). Returns true only when this exact
    // inconsistency holds: `GetPet()` is null but `GetPetGUID()` is not
    // empty.
    [[nodiscard]] bool HasStalePetSlot(Player* bot);

    // Clears the stale guid detected by `HasStalePetSlot` via the same
    // public `Unit::SetPetGUID(ObjectGuid::Empty)` setter the engine
    // itself would use if its own commented-out fix were live. Does
    // NOT touch a real, live pet -- only fires when `HasStalePetSlot`
    // is true. Returns false (no-op) otherwise.
    bool RequestClearStalePetSlot(Player* bot);

    // Searches for the nearest live, actually-tameable creature within
    // `range` (real grid search, `Cell::VisitObjects` -- the standard
    // AzerothCore idiom for "nearest object matching a predicate," same
    // one `WorldObject::FindNearestCreature` itself uses internally,
    // just without that function's fixed-entry restriction since "any
    // tameable beast," not one specific creature, is what auto-tame
    // needs). Delegates the actual tameability check to the real engine
    // predicate the spell itself uses
    // (`CreatureTemplate::IsTameable(bot->CanTameExoticPets())`) rather
    // than duplicating that logic -- this function's own job is only
    // "which nearby creature," not "is this creature tameable," so the
    // two can never disagree. Returns `nullptr` if none found; does not
    // move the bot (same division of responsibility as
    // `RequestTameBeast` below).
    [[nodiscard]] Creature* FindNearestTameableBeast(Player* bot, float range);

    // Casts Tame Beast at `target` via the same real, already-proven
    // `Combat::RequestCastSpell` primitive (real engine `Unit::CastSpell`,
    // full validation: range, LoS, `IsClass(CLASS_HUNTER, ...)`,
    // "already have a pet", "not a tameable beast", cast-time/GCD/moving
    // state). This function does NOT move the caster into range itself
    // -- unlike `Combat::RequestAttack`'s melee-chase behavior, a ranged
    // cast should not silently walk the bot into the target; the caller
    // (a future `GuideRuntime` step, or a debug command) is responsible
    // for positioning first, same division of responsibility as
    // `Combat::RequestCastSpell` already has for every other spell.
    //
    // Returns the real `SpellCastResult` -- `SPELL_CAST_OK` only means
    // the cast *started*; Tame Beast has a real cast time, and the pet
    // does not exist yet until it completes. Poll `BuildSnapshot` for
    // `HasPet=true` afterward, not this return value, to confirm
    // completion.
    SpellCastResult RequestTameBeast(Player* bot, Creature* target);

    // Casts Revive Pet, self-targeted, via `Combat::RequestCastSpell` --
    // real `Unit::CastSpell`. Deliberately does NOT require
    // `bot->GetPet()` to resolve first: the real effect handler this
    // spell runs, `Spell::EffectResurrectPet`, explicitly handles
    // `player->GetPet() == nullptr` itself by calling `SummonPet(0, ...)`
    // (which loads the pet from `PetStable`/DB regardless of whether a
    // live object currently exists) -- confirmed by reading
    // `SpellEffects.cpp` directly, and then **confirmed live**: cast
    // against a real `PetState::MissingDead` bot, the exact same tamed
    // pet (matching pet number) came back alive. An earlier version of
    // this function incorrectly bailed out with `SPELL_FAILED_BAD_TARGETS`
    // whenever `bot->GetPet()` was null, which skipped this exact
    // recovery path and led to a wrong "structural limitation" conclusion
    // before this was caught and fixed (`KNOWN_FAILURES.md` #13's final
    // writeup has the full story).
    //
    // Used by `Recovery::PlanPetRecovery` for both `PetState::ActiveDead`
    // and `PetState::MissingDead`. Returns the real `SpellCastResult`
    // from the engine, including `SPELL_FAILED_ALREADY_HAVE_SUMMON` if
    // the pet turns out to still be alive (harmless no-op, matching
    // `Combat::RequestCastSpell`'s "a rejected cast for a legitimate
    // reason is not an error" philosophy elsewhere in this module). This
    // is a low-level primitive only -- deciding *when* it's safe/correct
    // to call this (out of combat, spell actually learned) is
    // `Recovery::PlanPetRecovery`'s job (ADR-039), not this function's;
    // callers should go through `Combat::Execute(bot,
    // {IntentKind::RecoverPet})` via that policy rather than calling this
    // directly.
    SpellCastResult RequestRevivePet(Player* bot);

    // Casts Call Pet, self-targeted, the same way `RequestRevivePet`
    // does -- for `PetState::MissingAlive` specifically (a pet that's
    // real and recoverable but wasn't dead when it went missing).
    // **Not yet live-verified** -- see `CallPetSpellId`'s own caveat.
    // `Recovery::PlanPetRecovery` gates this on `Player::HasSpell` for
    // that reason, same discipline as every other gated ability in this
    // project.
    SpellCastResult RequestCallPet(Player* bot);

    // Sets `bot`'s current pet to `state` via the same real, public
    // `Unit::SetReactState` the engine itself uses for pet command-bar
    // clicks. A freshly tamed pet defaults to `REACT_PASSIVE` (confirmed
    // live) -- it will NOT automatically assist in combat until commanded,
    // exactly like a real human player's freshly tamed pet requires a
    // manual react-state change or explicit "attack" command. Returns
    // false (no-op) if `bot` has no live pet.
    bool RequestSetPetReactState(Player* bot, ReactStates state);

    // Issues the same explicit pet-attack command a player sends from the
    // pet action bar. This keeps target ownership with the engagement
    // planner: the pet assists on the selected target instead of roaming
    // for targets through REACT_AGGRESSIVE.
    bool RequestAttackTarget(Player* bot, ObjectGuid const& targetGuid);
} // namespace AutonomousPlayer::Pets

#endif // AUTONOMOUS_PLAYER_BOT_PETS_H
