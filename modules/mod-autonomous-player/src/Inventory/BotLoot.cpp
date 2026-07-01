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

#include "BotLoot.h"
#include "Creature.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Inventory
{
    bool LootCorpse(Player* bot, Creature* corpse)
    {
        if (!bot || !bot->GetSession() || !corpse)
        {
            return false;
        }

        WorldSession* session = bot->GetSession();
        ObjectGuid guid = corpse->GetGUID();

        WorldPacket lootPacket(CMSG_LOOT, 8);
        lootPacket << guid;
        session->HandleLootOpcode(lootPacket);

        // Read the slot count directly off the live Loot object -- see
        // header comment for why (no client to parse our own outgoing
        // SMSG_LOOT_RESPONSE).
        std::size_t itemCount = corpse->loot.items.size();
        for (std::size_t slot = 0; slot < itemCount; ++slot)
        {
            WorldPacket storePacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            storePacket << uint8(slot);
            session->HandleAutostoreLootItemOpcode(storePacket);
        }

        if (corpse->loot.gold > 0)
        {
            WorldPacket moneyPacket(CMSG_LOOT_MONEY, 0);
            session->HandleLootMoneyOpcode(moneyPacket);
        }

        WorldPacket releasePacket(CMSG_LOOT_RELEASE, 8);
        releasePacket << guid;
        session->HandleLootReleaseOpcode(releasePacket);

        return true;
    }
} // namespace AutonomousPlayer::Inventory
