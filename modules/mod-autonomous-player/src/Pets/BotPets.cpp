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

#include "BotPets.h"
#include "CharmInfo.h"
#include "Combat/BotCombat.h"
#include "Creature.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Pets
{
    PetSnapshot BuildSnapshot(Player* bot)
    {
        PetSnapshot snapshot;
        if (!bot)
        {
            return snapshot;
        }

        Pet* pet = bot->GetPet();
        if (!pet)
        {
            return snapshot;
        }

        snapshot.HasPet = true;
        snapshot.Guid = pet->GetGUID();
        snapshot.Entry = pet->GetEntry();
        snapshot.Alive = pet->IsAlive();
        snapshot.Health = pet->GetHealth();
        snapshot.MaxHealth = pet->GetMaxHealth();
        snapshot.React = pet->GetReactState();
        if (Unit* victim = pet->GetVictim())
        {
            snapshot.VictimGuid = victim->GetGUID();
        }
        return snapshot;
    }

    PetState ClassifyPetState(PetSnapshot const& snapshot, ObjectGuid const& lastKnownPetGuid)
    {
        if (snapshot.HasPet)
        {
            return snapshot.Alive ? PetState::Alive : PetState::Dead;
        }

        return lastKnownPetGuid.IsEmpty() ? PetState::NotYetTamed : PetState::Dismissed;
    }

    SpellCastResult RequestTameBeast(Player* bot, Creature* target)
    {
        if (!bot || !target)
        {
            return SPELL_FAILED_BAD_TARGETS;
        }

        return Combat::RequestCastSpell(bot, target, TameBeastSpellId);
    }

    SpellCastResult RequestRevivePet(Player* bot)
    {
        if (!bot)
        {
            return SPELL_FAILED_BAD_TARGETS;
        }

        Pet* pet = bot->GetPet();
        if (!pet)
        {
            // `GetPet()` still resolves a dead-but-not-yet-dismissed pet
            // (confirmed by reading `Player::GetPet()`'s real
            // implementation -- it only checks the summon slot guid, not
            // alive state) -- reaching here means there is genuinely no
            // pet object at all to revive, not just a dead one.
            return SPELL_FAILED_BAD_TARGETS;
        }

        return Combat::RequestCastSpell(bot, pet, RevivePetSpellId);
    }

    bool RequestSetPetReactState(Player* bot, ReactStates state)
    {
        if (!bot)
        {
            return false;
        }

        Pet* pet = bot->GetPet();
        if (!pet)
        {
            return false;
        }

        pet->SetReactState(state);
        return true;
    }

    bool RequestAttackTarget(Player* bot, ObjectGuid const& targetGuid)
    {
        if (!bot || !bot->GetSession() || targetGuid.IsEmpty())
        {
            return false;
        }

        Pet* pet = bot->GetPet();
        if (!pet || !pet->IsAlive())
        {
            return false;
        }

        WorldPacket packet(CMSG_PET_ACTION, 20);
        packet << pet->GetGUID();
        packet << uint32(MAKE_UNIT_ACTION_BUTTON(COMMAND_ATTACK, ACT_COMMAND));
        packet << targetGuid;
        bot->GetSession()->HandlePetAction(packet);
        return true;
    }
} // namespace AutonomousPlayer::Pets
