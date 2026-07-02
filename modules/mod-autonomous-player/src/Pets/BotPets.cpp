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
#include "PetDefines.h"
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

    PetState ClassifyPetState(Player* bot, PetSnapshot const& snapshot, ObjectGuid const& lastKnownPetGuid)
    {
        if (snapshot.HasPet)
        {
            return snapshot.Alive ? PetState::ActiveAlive : PetState::ActiveDead;
        }

        // GetPet() found nothing live -- check the real pet stable
        // before concluding there's genuinely no pet to recover. This is
        // exactly the read `KNOWN_FAILURES.md` #13 was missing the first
        // time: presence in `UnslottedPets` (not just `HasPet`) is what
        // actually determines whether `RequestRevivePet`/`RequestCallPet`
        // have anything real to act on.
        if (bot)
        {
            if (PetStable const* stable = bot->GetPetStable())
            {
                if (PetStable::PetInfo const* info = stable->GetUnslottedHunterPet())
                {
                    return info->Health > 0 ? PetState::MissingAlive : PetState::MissingDead;
                }
            }
        }

        return lastKnownPetGuid.IsEmpty() ? PetState::NoPet : PetState::Dismissed;
    }

    bool HasStalePetSlot(Player* bot)
    {
        if (!bot)
        {
            return false;
        }

        return !bot->GetPet() && !bot->GetPetGUID().IsEmpty();
    }

    bool RequestClearStalePetSlot(Player* bot)
    {
        if (!HasStalePetSlot(bot))
        {
            return false;
        }

        bot->SetPetGUID(ObjectGuid::Empty);
        return true;
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

        // Deliberately self-targeted, whether or not a live `Pet*`
        // currently resolves. Confirmed by reading the real effect
        // handler this spell actually runs, `Spell::EffectResurrectPet`
        // (SpellEffects.cpp): it reads `player->GetPet()` internally and
        // explicitly branches on it being null --
        // `player->SummonPet(0, ..., SUMMON_PET, 0ms, damage)`, which
        // (per that function's own comment) loads the pet from
        // `PetStable`/`Pet::LoadPetFromDB` regardless of whether a live
        // object exists right now. An earlier version of this function
        // bailed out with `SPELL_FAILED_BAD_TARGETS` whenever
        // `bot->GetPet()` was null, incorrectly assuming a live pet
        // object was required -- that skipped the exact code path this
        // spell exists for (reviving a pet that isn't currently loaded
        // at all, not just one that's dead-in-place).
        return Combat::RequestCastSpell(bot, bot, RevivePetSpellId);
    }

    SpellCastResult RequestCallPet(Player* bot)
    {
        if (!bot)
        {
            return SPELL_FAILED_BAD_TARGETS;
        }

        return Combat::RequestCastSpell(bot, bot, CallPetSpellId);
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
