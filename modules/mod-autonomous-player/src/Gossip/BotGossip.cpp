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

#include "BotGossip.h"
#include "Creature.h"
#include "GossipDef.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Gossip
{
    bool RequestGossipHello(Player* bot, Creature* npc)
    {
        if (!bot || !bot->GetSession() || !npc)
        {
            return false;
        }

        WorldPacket packet(CMSG_GOSSIP_HELLO, 8);
        packet << npc->GetGUID();

        bot->GetSession()->HandleGossipHelloOpcode(packet);

        return true;
    }

    std::optional<uint32_t> FindGossipOptionIndex(Player* bot, uint32_t optionType)
    {
        if (!bot)
        {
            return std::nullopt;
        }

        for (auto const& [id, item] : bot->PlayerTalkClass->GetGossipMenu().GetMenuItems())
        {
            if (item.OptionType == optionType)
            {
                return id;
            }
        }

        return std::nullopt;
    }

    bool RequestGossipSelectOption(Player* bot, Creature* npc, uint32_t gossipListId)
    {
        if (!bot || !bot->GetSession() || !npc)
        {
            return false;
        }

        WorldPacket packet(CMSG_GOSSIP_SELECT_OPTION, 16);
        packet << npc->GetGUID();
        packet << uint32(bot->PlayerTalkClass->GetGossipMenu().GetMenuId());
        packet << uint32(gossipListId);

        bot->GetSession()->HandleGossipSelectOptionOpcode(packet);

        return true;
    }
} // namespace AutonomousPlayer::Gossip
