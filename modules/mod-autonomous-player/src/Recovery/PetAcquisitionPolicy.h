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

#ifndef AUTONOMOUS_PLAYER_PET_ACQUISITION_POLICY_H
#define AUTONOMOUS_PLAYER_PET_ACQUISITION_POLICY_H

#include "Combat/CombatIntent.h"
#include "Pets/BotPets.h"
#include <optional>

class Player;

namespace AutonomousPlayer::Recovery
{
    // Deliberately separate from `PlanPetRecovery` (ADR-039/040) -- real,
    // distinct scope: acquiring a *first* pet is not "recovering" one
    // that already exists in the stable. Only ever applies to
    // `Pets::PetState::NoPet` -- `Dismissed`/`Missing*` pets are real,
    // already-owned pets that belong to `PlanPetRecovery` instead;
    // auto-re-taming a Hunter who merely dismissed or lost track of an
    // existing pet is explicitly out of scope here (and would be the
    // wrong action -- their existing pet is still real and recoverable).
    //
    // Slice 1, deliberately minimal (`ARCHITECTURE.md` ADR-041): returns
    // an `AcquirePet` intent only when a real, live, actually-tameable
    // creature (`Pets::FindNearestTameableBeast`, which delegates the
    // tameability check itself to `CreatureTemplate::IsTameable` -- the
    // same predicate the real spell's own `CheckCast` uses, so this
    // function can never disagree with the engine about what's
    // tameable) is already within Tame Beast's own real cast range
    // (`SpellInfo::GetMaxRange`) -- no new approach/movement logic this
    // slice, matching `Pets::RequestTameBeast`'s existing "caller
    // positions first" division of responsibility. A beast further away
    // is not chased; this fires opportunistically as one naturally comes
    // into range during a guide's own normal movement, same spirit as
    // `PlanPetRecovery`'s gating. Real movement toward a found-but-
    // distant beast is real, separate, later scope if this proves out.
    //
    // Gated on `bot` not in combat and `IsClass(CLASS_HUNTER, ...)` --
    // deliberately NOT `Player::HasSpell(Pets::TameBeastSpellId)`, found
    // live to be the wrong check (`KNOWN_FAILURES.md` #15): Tame Beast is
    // a real, innate Hunter ability, not one granted through the normal
    // trainer/spellbook system -- `HasSpell` returns false for it even on
    // a Hunter who can genuinely cast it, which would have silently
    // disabled this feature for every Hunter, forever, had it shipped
    // that way. See `PetAcquisitionPolicy.cpp` for the live evidence.
    [[nodiscard]] std::optional<Combat::CombatIntent> PlanPetAcquisition(Player* bot, Pets::PetState state);
} // namespace AutonomousPlayer::Recovery

#endif // AUTONOMOUS_PLAYER_PET_ACQUISITION_POLICY_H
