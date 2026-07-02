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
    // 2. `IsClass(CLASS_HUNTER, CLASS_CONTEXT_ABILITY)` -- deliberately
    //    NOT `bot->HasSpell(...)` for either Revive Pet or Call Pet
    //    (`KNOWN_FAILURES.md` #15/#16): both are real, innate Hunter
    //    abilities, not ones granted through the normal spellbook
    //    system -- confirmed live for both (casting either against a
    //    Hunter whose spellbook lists neither id still returned the
    //    real `SPELL_FAILED_ALREADY_HAVE_SUMMON`, not an unknown-spell
    //    rejection, while she had an active pet). An earlier version
    //    gated each branch on `HasSpell` and would have been a silent,
    //    permanent no-op for every Hunter -- caught and fixed before
    //    that gap went unnoticed.
    // 3. `PetState::ActiveDead` or `PetState::MissingDead`: returns
    //    `RecoverPet` (`Pets::RequestRevivePet`). Both states use the
    //    same primitive because the real effect handler behind it,
    //    `Spell::EffectResurrectPet`, handles both a live-but-dead `Pet*`
    //    and a not-currently-loaded pet identically (confirmed live for
    //    `MissingDead`: the exact same tamed pet, matching pet number,
    //    came back alive).
    // 4. `PetState::MissingAlive`: returns `CallPet`
    //    (`Pets::RequestCallPet`). The gated cast itself
    //    (`SPELL_FAILED_ALREADY_HAVE_SUMMON` against an active pet) is
    //    confirmed real; the specific `MissingAlive` -> `Alive` recovery
    //    transition has not yet been directly observed -- no test Hunter
    //    has been put into that exact state this session.
    //
    // Returns `std::nullopt` otherwise -- including for `PetState::
    // Dismissed`/`NoPet`/`ActiveAlive`, deliberately: auto-re-taming
    // stays out of scope for this function.
    [[nodiscard]] std::optional<Combat::CombatIntent> PlanPetRecovery(Player* bot, Pets::PetState state);
} // namespace AutonomousPlayer::Recovery

#endif // AUTONOMOUS_PLAYER_PET_RECOVERY_POLICY_H
