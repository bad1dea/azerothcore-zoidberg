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
#include "GameObject.h"
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

        // Quest drops live in a SEPARATE per-player list, not
        // `loot.items` -- found live (`KNOWN_FAILURES.md` #26, ADR-048's
        // first full grind run): 8+ verified kill+loot cycles collected
        // ZERO Plainstrider Meat because these slots were never
        // requested. The real protocol addresses a player's own quest
        // items as `items.size() + <index in that player's quest list>`
        // (the same encoding `SMSG_LOOT_RESPONSE` sends a real client,
        // `LootMgr.cpp`'s `items.size() + (qi - q_list->begin())`, and
        // what `Loot::LootItemInSlot` decodes). Same real opcode handler
        // as above; per-player visibility/looted-flag rules still apply
        // for real inside it.
        auto const& questItemMap = corpse->loot.GetPlayerQuestItems();
        auto questItems = questItemMap.find(bot->GetGUID());
        std::size_t questItemCount =
            (questItems != questItemMap.end() && questItems->second) ? questItems->second->size() : 0;
        for (std::size_t index = 0; index < questItemCount; ++index)
        {
            WorldPacket storePacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            storePacket << uint8(itemCount + index);
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

    bool LootGameObject(Player* bot, GameObject* gameObject)
    {
        if (!bot || !bot->GetSession() || !gameObject)
            return false;

        WorldSession* session = bot->GetSession();
        ObjectGuid const guid = gameObject->GetGUID();
        std::size_t const itemCount = gameObject->loot.items.size();
        for (std::size_t slot = 0; slot < itemCount; ++slot)
        {
            WorldPacket storePacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            storePacket << uint8(slot);
            session->HandleAutostoreLootItemOpcode(storePacket);
        }

        auto const& questItemMap = gameObject->loot.GetPlayerQuestItems();
        auto const questItems = questItemMap.find(bot->GetGUID());
        std::size_t const questItemCount =
            (questItems != questItemMap.end() && questItems->second) ? questItems->second->size() : 0;
        for (std::size_t index = 0; index < questItemCount; ++index)
        {
            WorldPacket storePacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            storePacket << uint8(itemCount + index);
            session->HandleAutostoreLootItemOpcode(storePacket);
        }
        if (gameObject->loot.gold)
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
