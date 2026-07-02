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
    // maintenance (ADR-039, corrected/expanded same day -- see
    // `KNOWN_FAILURES.md` #13's full writeup for how the first version
    // of this got the actual recovery mechanics wrong before being
    // caught and fixed). Deciding *whether and when* recovering the pet
    // is the right thing to do, kept separate from *how* it's done
    // (`Pets::RequestRevivePet`/`Pets::RequestCallPet`/
    // `Pets::RequestClearStalePetSlot`) and separate from *when a step
    // gets to run at all* (`GuideRuntime::Tick`, which calls this before
    // dispatching to the current guide step).
    //
    // Checks, in order (all gated on `!bot->IsInCombat()` -- reviving/
    // calling mid-fight is both unsafe, a real player wouldn't do it
    // while being hit, and safety should be an explicit check here, not
    // an incidental engine rejection):
    // 1. `Pets::HasStalePetSlot(bot)` (`KNOWN_FAILURES.md` #13, the
    //    narrower, real but ultimately not-the-cause finding): a stale
    //    `Unit::GetPetGUID()` left non-empty while `GetPet()` resolves to
    //    null blocks `EffectTameCreature` outright. Returns
    //    `ClearStalePetSlot` immediately if found.
    // 2. `PetState::ActiveDead` or `PetState::MissingDead`: returns
    //    `RecoverPet` (`Pets::RequestRevivePet`) if `bot->HasSpell`
    //    confirms Revive Pet is actually learned. Both states use the
    //    same primitive because the real effect handler behind it,
    //    `Spell::EffectResurrectPet`, handles both a live-but-dead `Pet*`
    //    and a not-currently-loaded pet identically (confirmed live for
    //    `MissingDead`: the exact same tamed pet, matching pet number,
    //    came back alive).
    // 3. `PetState::MissingAlive`: returns `CallPet`
    //    (`Pets::RequestCallPet`) if `bot->HasSpell` confirms Call Pet is
    //    learned. **Not yet live-verified this session** -- no test
    //    Hunter reached the level to learn it; see `CallPetSpellId`'s own
    //    caveat.
    //
    // Returns `std::nullopt` otherwise -- including for `PetState::
    // Dismissed`/`NoPet`/`ActiveAlive`, and for a gated-but-not-yet-
    // learned ability, deliberately: auto-re-taming stays out of scope
    // for this function, and this project never assumes a spell id is
    // usable without `HasSpell` confirming it live.
    [[nodiscard]] std::optional<Combat::CombatIntent> PlanPetRecovery(Player* bot, Pets::PetState state);
} // namespace AutonomousPlayer::Recovery

#endif // AUTONOMOUS_PLAYER_PET_RECOVERY_POLICY_H
