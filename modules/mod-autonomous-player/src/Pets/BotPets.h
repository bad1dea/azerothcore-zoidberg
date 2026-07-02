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
    };

    // Builds a snapshot of `bot->GetPet()` right now. Safe to call
    // whether or not a pet exists.
    [[nodiscard]] PetSnapshot BuildSnapshot(Player* bot);

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

    // Sets `bot`'s current pet to `state` via the same real, public
    // `Unit::SetReactState` the engine itself uses for pet command-bar
    // clicks. A freshly tamed pet defaults to `REACT_PASSIVE` (confirmed
    // live) -- it will NOT automatically assist in combat until commanded,
    // exactly like a real human player's freshly tamed pet requires a
    // manual react-state change or explicit "attack" command. Returns
    // false (no-op) if `bot` has no live pet.
    bool RequestSetPetReactState(Player* bot, ReactStates state);
} // namespace AutonomousPlayer::Pets

#endif // AUTONOMOUS_PLAYER_BOT_PETS_H
