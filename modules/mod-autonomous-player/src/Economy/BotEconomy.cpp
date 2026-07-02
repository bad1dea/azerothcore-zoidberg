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

#include "BotEconomy.h"
#include "Bag.h"
#include "Creature.h"
#include "Item.h"
#include "ItemPackets.h"
#include "ItemTemplate.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <vector>

namespace AutonomousPlayer::Economy
{
    bool BuyItem(Player* bot, Creature* vendor, uint32_t itemId, uint32_t count)
    {
        if (!bot || !bot->GetSession() || !vendor)
        {
            return false;
        }

        VendorItemData const* items = vendor->GetVendorItems();
        if (!items)
        {
            return false;
        }

        int32_t slot = -1;
        for (uint32_t i = 0; i < items->GetItemCount(); ++i)
        {
            VendorItem const* vendorItem = items->GetItem(i);
            if (vendorItem && vendorItem->item == itemId)
            {
                slot = static_cast<int32_t>(i);
                break;
            }
        }

        if (slot < 0)
        {
            return false;
        }

        WorldPackets::Item::BuyItem packet{WorldPacket{CMSG_BUY_ITEM}};
        packet.VendorGuid = vendor->GetGUID();
        packet.Item = itemId;
        // The handler subtracts 1 (client always sends vendorSlot + 1);
        // see WorldSession::HandleBuyItemOpcode.
        packet.Slot = static_cast<uint32>(slot) + 1;
        packet.Count = count;

        bot->GetSession()->HandleBuyItemOpcode(packet);

        return true;
    }

    namespace
    {
        // Visits every item in the backpack and the equipped bags --
        // the same slots a real player empties at a vendor. Equipment,
        // bank, keyring, and the bags themselves are not visited.
        template <typename Visitor>
        void ForEachCarriedItem(Player* bot, Visitor&& visit)
        {
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            {
                if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    visit(item);
                }
            }

            for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            {
                Bag* bag = bot->GetBagByPos(bagSlot);
                if (!bag)
                {
                    continue;
                }

                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                {
                    if (Item* item = bag->GetItemByPos(i))
                    {
                        visit(item);
                    }
                }
            }
        }

        bool IsSellableGray(Item const* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            return proto && proto->Quality == ITEM_QUALITY_POOR && proto->SellPrice > 0;
        }
    } // namespace

    uint32_t CountSellableGrayItems(Player* bot)
    {
        if (!bot)
        {
            return 0;
        }

        uint32_t count = 0;
        ForEachCarriedItem(bot, [&count](Item* item)
        {
            if (IsSellableGray(item))
            {
                ++count;
            }
        });

        return count;
    }

    uint32_t CountFreeBagSlots(Player* bot)
    {
        if (!bot)
        {
            return 0;
        }

        uint32_t freeSlots = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                ++freeSlots;
            }
        }

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            if (Bag* bag = bot->GetBagByPos(bagSlot))
            {
                freeSlots += bag->GetFreeSlots();
            }
        }

        return freeSlots;
    }

    uint32_t SellGrayItems(Player* bot, Creature* vendor)
    {
        if (!bot || !bot->GetSession() || !vendor)
        {
            return 0;
        }

        // Collect guids first, then send -- each handled sell mutates
        // the inventory (item moves to the vendor's buyback list), so
        // requests are not issued while still iterating the slots.
        std::vector<ObjectGuid> toSell;
        ForEachCarriedItem(bot, [&toSell](Item* item)
        {
            if (IsSellableGray(item))
            {
                toSell.push_back(item->GetGUID());
            }
        });

        for (ObjectGuid const& itemGuid : toSell)
        {
            WorldPackets::Item::SellItem packet{WorldPacket{CMSG_SELL_ITEM}};
            packet.VendorGuid = vendor->GetGUID();
            packet.ItemGuid = itemGuid;
            packet.Count = 0; // 0 == sell the whole stack (handler-defined)

            bot->GetSession()->HandleSellItemOpcode(packet);
        }

        return static_cast<uint32_t>(toSell.size());
    }

    bool RepairAll(Player* bot, Creature* vendor)
    {
        if (!bot || !bot->GetSession() || !vendor)
        {
            return false;
        }

        WorldPacket packet(CMSG_REPAIR_ITEM, 17);
        packet << vendor->GetGUID();
        packet << ObjectGuid::Empty; // empty item guid == repair everything
        packet << uint8(0);          // guildBank = false

        bot->GetSession()->HandleRepairItemOpcode(packet);

        return true;
    }
} // namespace AutonomousPlayer::Economy
