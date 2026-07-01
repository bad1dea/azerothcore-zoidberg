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

#include "BotGrowth.h"
#include "Creature.h"
#include "NPCPackets.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Growth
{
    bool RequestTrainerList(Player* bot, Creature* trainer)
    {
        if (!bot || !bot->GetSession() || !trainer)
        {
            return false;
        }

        WorldPackets::NPC::Hello packet{WorldPacket{CMSG_TRAINER_LIST}};
        packet.Unit = trainer->GetGUID();

        bot->GetSession()->HandleTrainerListOpcode(packet);

        return true;
    }

    std::optional<uint32_t> FindLearnableTrainerSpell(Player* bot, Creature* trainer)
    {
        if (!bot || !trainer)
        {
            return std::nullopt;
        }

        Trainer::Trainer const* trainerData = sObjectMgr->GetTrainer(trainer->GetEntry());
        if (!trainerData)
        {
            return std::nullopt;
        }

        for (Trainer::Spell const& spell : trainerData->GetSpells())
        {
            if (trainerData->CanTeachSpell(bot, &spell))
            {
                return spell.SpellId;
            }
        }

        return std::nullopt;
    }

    bool RequestLearnSpell(Player* bot, Creature* trainer, uint32_t spellId)
    {
        if (!bot || !bot->GetSession() || !trainer)
        {
            return false;
        }

        WorldPackets::NPC::TrainerBuySpell packet{WorldPacket{CMSG_TRAINER_BUY_SPELL}};
        packet.TrainerGUID = trainer->GetGUID();
        packet.SpellID = static_cast<int32>(spellId);

        bot->GetSession()->HandleTrainerBuySpellOpcode(packet);

        return true;
    }
} // namespace AutonomousPlayer::Growth
