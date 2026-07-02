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

#include "PetRecoveryPolicy.h"
#include "Pets/BotPets.h"
#include "Player.h"

namespace AutonomousPlayer::Recovery
{
    std::optional<Combat::CombatIntent> PlanPetRecovery(Player* bot, Pets::PetState state)
    {
        if (!bot || bot->IsInCombat())
        {
            return std::nullopt;
        }

        // ADR-040 fix, found live: `Combat::Execute`'s `RecoverPet`/
        // `CallPet` cases fire-and-forget `Unit::CastSpell`, which -- like
        // a real player mashing the same spell button -- interrupts and
        // restarts an already-in-progress cast rather than being a no-op.
        // `GuideRuntime::Tick` calls this every eligible tick, so without
        // this check a real cast (both Revive Pet and Call Pet have a
        // real cast time) would never survive long enough to complete:
        // each subsequent tick's re-issued intent would cancel the
        // previous attempt before it finished. `Unit::IsNonMeleeSpellCast`
        // is the real, standard engine query for "is this unit currently
        // mid-cast" -- checking it here means a cast already in flight is
        // simply left alone instead of being restarted every tick.
        if (bot->IsNonMeleeSpellCast(false))
        {
            return std::nullopt;
        }

        // KNOWN_FAILURES.md #13: real but narrower than first thought --
        // still checked and fixed here whenever found, but not the cause
        // of that finding's actual re-taming rejection.
        if (Pets::HasStalePetSlot(bot))
        {
            return Combat::CombatIntent{ Combat::IntentKind::ClearStalePetSlot, ObjectGuid::Empty, 0 };
        }

        // Deliberately NOT `bot->HasSpell(...)` for either branch below
        // -- found live to be the wrong check (`KNOWN_FAILURES.md` #15,
        // #16): Revive Pet and Call Pet, like Tame Beast, are real,
        // innate Hunter abilities, not ones granted through the normal
        // trainer/spellbook system. Confirmed live for both: casting
        // spell 982 (Revive Pet) and spell 883 (Call Pet) against a
        // Hunter whose spellbook lists neither id both returned the
        // real, specific `SPELL_FAILED_ALREADY_HAVE_SUMMON` (not an
        // unknown-spell rejection) while she had an active pet -- proof
        // both are genuinely castable regardless of `HasSpell`. Had this
        // shipped with the `HasSpell` gate on either branch, that branch
        // would have been a silent, permanent no-op for every Hunter,
        // ever -- this project's own earlier "verified" claims about
        // `RecoverPet`'s automatic firing (ADR-040) turned out to have
        // only ever tested the raw `Pets::RequestRevivePet` primitive
        // via the manual `revivepet` debug command, never the policy's
        // gate actually passing in practice -- caught and corrected
        // before that gap was left unnoticed.
        if (!bot->IsClass(CLASS_HUNTER, CLASS_CONTEXT_ABILITY))
        {
            return std::nullopt;
        }

        switch (state)
        {
            case Pets::PetState::ActiveDead:
            case Pets::PetState::MissingDead:
                return Combat::CombatIntent{ Combat::IntentKind::RecoverPet, ObjectGuid::Empty, 0 };

            case Pets::PetState::MissingAlive:
                return Combat::CombatIntent{ Combat::IntentKind::CallPet, ObjectGuid::Empty, 0 };

            case Pets::PetState::NoPet:
            case Pets::PetState::Dismissed:
            case Pets::PetState::ActiveAlive:
                return std::nullopt;
        }

        return std::nullopt;
    }
} // namespace AutonomousPlayer::Recovery
