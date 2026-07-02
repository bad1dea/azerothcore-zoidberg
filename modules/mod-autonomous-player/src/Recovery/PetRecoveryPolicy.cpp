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
        if (!bot || state != Pets::PetState::Dead)
        {
            return std::nullopt;
        }

        if (bot->IsInCombat())
        {
            return std::nullopt;
        }

        if (!bot->HasSpell(Pets::RevivePetSpellId))
        {
            return std::nullopt;
        }

        return Combat::CombatIntent{ Combat::IntentKind::RecoverPet, ObjectGuid::Empty, 0 };
    }
} // namespace AutonomousPlayer::Recovery
