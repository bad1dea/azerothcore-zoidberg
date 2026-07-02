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

    // Coarse pet lifecycle state (ADR-039), used to decide whether pet
    // recovery is even applicable right now. `Player::GetPet()` alone
    // cannot distinguish "never tamed anything" from "had a pet, it's
    // now gone without ever being observed dead" (a real dismiss, or an
    // unexpected despawn) -- both just read as `HasPet=false`. Rather
    // than inventing a state the engine can't actually support,
    // `ClassifyPetState` takes the caller's own last-known pet guid
    // (owned by `GuideRuntime::BotGuideState`, not hidden in this
    // component) as an explicit parameter, staying a pure function (same
    // tick-safety discipline as `BuildSnapshot`/`EncounterModel`).
    enum class PetState : uint8_t
    {
        NotYetTamed, // HasPet=false and no last-known guid at all
        Dismissed,   // HasPet=false but a last-known guid was recorded
        Dead,        // HasPet=true, Alive=false
        Alive,       // HasPet=true, Alive=true
    };

    [[nodiscard]] PetState ClassifyPetState(PetSnapshot const& snapshot, ObjectGuid const& lastKnownPetGuid);

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

    // Casts Revive Pet on `bot`'s own dead pet via `Combat::RequestCastSpell`
    // (real `Unit::CastSpell`, self-targeted -- Revive Pet has no
    // separate unit target, it acts on `bot->GetPet()` internally).
    // Returns `SPELL_FAILED_BAD_TARGETS` if `bot` has no pet at all (no
    // point issuing a doomed cast); otherwise the real `SpellCastResult`
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
