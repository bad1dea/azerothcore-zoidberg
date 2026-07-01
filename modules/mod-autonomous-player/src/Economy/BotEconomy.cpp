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
#include "Creature.h"
#include "ItemPackets.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

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
