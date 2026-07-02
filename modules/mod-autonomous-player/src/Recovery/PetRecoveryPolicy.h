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

#ifndef AUTONOMOUS_PLAYER_PET_RECOVERY_POLICY_H
#define AUTONOMOUS_PLAYER_PET_RECOVERY_POLICY_H

#include "Combat/CombatIntent.h"
#include "Pets/BotPets.h"
#include <optional>

class Player;

namespace AutonomousPlayer::Recovery
{
    // The "Recover" stage of the Singular pull-transaction model
    // (`HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`, ADR-022) applied to pet
    // maintenance (ADR-039) -- deciding *whether and when* recovering the
    // pet is the right thing to do, kept separate from *how* it's done
    // (`Pets::RequestRevivePet`) and separate from *when a step gets to
    // run at all* (`GuideRuntime::Tick`, which calls this before
    // dispatching to the current guide step).
    //
    // Returns a `RecoverPet` intent only when all of these hold:
    // - the pet is actually dead (`Pets::PetState::Dead`) -- a missing/
    //   dismissed/never-tamed pet is NOT auto-re-acquired by this
    //   function; that's separate, explicitly out-of-scope future work
    //   (see `HANDOFF.md` NEXT TASK).
    // - `bot` is not in combat (`Unit::IsInCombat()`) -- reviving mid-
    //   fight is both unsafe (a real player wouldn't do it while being
    //   hit) and, per this fork's engine data, likely already rejected
    //   for other reasons; checking explicitly here documents the
    //   safety intent rather than relying on an incidental engine
    //   rejection.
    // - `bot` has actually learned Revive Pet (`Player::HasSpell`) --
    //   NOT assumed just because the spell id is theoretically real
    //   (confirmed live, ADR-036/039's methodology): a low-level Hunter
    //   may not have it yet, the same class of gated-ability finding
    //   this project already made for Priest/Warrior spells
    //   (`KNOWN_FAILURES.md` Gate 2).
    //
    // Returns `std::nullopt` otherwise -- including for
    // `PetState::Dismissed`/`NotYetTamed`/`Alive`, deliberately: this
    // function's only job is the "revive a dead pet" recovery case.
    [[nodiscard]] std::optional<Combat::CombatIntent> PlanPetRecovery(Player* bot, Pets::PetState state);
} // namespace AutonomousPlayer::Recovery

#endif // AUTONOMOUS_PLAYER_PET_RECOVERY_POLICY_H
