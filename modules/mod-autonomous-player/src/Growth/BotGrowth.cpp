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
#include "Bag.h"
#include "Creature.h"
#include "Item.h"
#include "ItemPackets.h"
#include "ItemTemplate.h"
#include "NPCPackets.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "Trainer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <set>
#include <vector>

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

    namespace
    {
        // Visits the backpack and every equipped bag -- same shape as
        // the Economy component's vendor visitor (deliberately local
        // to each component; the two policies evolve independently).
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
    } // namespace

    uint32_t EquipBagUpgrades(Player* bot)
    {
        if (!bot || !bot->GetSession())
        {
            return 0;
        }

        // Collect candidate guids first -- each real equip mutates the
        // inventory mid-iteration otherwise (same rule as SellGrayItems).
        std::vector<ObjectGuid> candidates;
        std::set<ObjectGuid> bagCandidates;
        ForEachCarriedItem(bot, [bot, &candidates, &bagCandidates](Item* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP)
            {
                return;
            }
            bool const isBag = (proto->Class == ITEM_CLASS_CONTAINER);
            if (!isBag && proto->Class != ITEM_CLASS_WEAPON
                && proto->Class != ITEM_CLASS_ARMOR)
            {
                return;
            }

            uint8 slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot == NULL_SLOT)
            {
                return;
            }

            Item const* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (equipped)
            {
                if (isBag)
                {
                    // Bags: only fill an EMPTY slot or replace a strictly
                    // smaller bag. Never swap an equal/bigger bag -- that
                    // would shuffle its contents for no gain. (More
                    // capacity = fewer forced sell trips mid-grind; a
                    // looted bag was previously left unused in the pack.)
                    if (equipped->GetTemplate()->ContainerSlots >= proto->ContainerSlots)
                    {
                        return;
                    }
                }
                else if (equipped->GetTemplate()->ItemLevel >= proto->ItemLevel)
                {
                    return;
                }
            }

            candidates.push_back(item->GetGUID());
            if (isBag)
            {
                bagCandidates.insert(item->GetGUID());
            }
        });

        uint32_t equippedCount = 0;
        for (ObjectGuid const& guid : candidates)
        {
            // Re-resolve: an earlier equip in this loop may have moved
            // (or two-hand-displaced) this item already.
            Item* item = bot->GetItemByGuid(guid);
            if (!item || item->IsEquipped())
            {
                continue;
            }

            if (bagCandidates.count(guid))
            {
                // Bags do NOT go through CMSG_AUTOEQUIP_ITEM (that path
                // only equips weapons/armor -- observed live: 6 looted
                // bags sat unequipped in the backpack). Equip into a
                // free bag slot via the real storage swap the client
                // uses for drag-to-bagslot.
                uint8 slot = bot->FindEquipSlot(item->GetTemplate(), NULL_SLOT, true);
                if (slot == NULL_SLOT)
                {
                    continue;
                }
                uint16 dst = (uint16(INVENTORY_SLOT_BAG_0) << 8) | slot;
                bot->SwapItem(item->GetPos(), dst);
                Item* after = bot->GetItemByGuid(guid);
                if (after && after->IsEquipped())
                {
                    ++equippedCount;
                }
                continue;
            }

            WorldPackets::Item::AutoEquipItem packet{WorldPacket{CMSG_AUTOEQUIP_ITEM}};
            packet.SourceBag = item->GetBagSlot();
            packet.SourceSlot = item->GetSlot();
            bot->GetSession()->HandleAutoEquipItemOpcode(packet);

            // Verified against real item state -- the handler reports
            // rejections only to the (headless) client session.
            Item* after = bot->GetItemByGuid(guid);
            if (after && after->IsEquipped())
            {
                ++equippedCount;
            }
        }

        return equippedCount;
    }
} // namespace AutonomousPlayer::Growth
