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
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <map>
#include <utility>
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

    uint32_t BuyGearUpgrades(Player* bot, Creature* vendor)
    {
        if (!bot || !bot->GetSession() || !vendor)
        {
            return 0;
        }
        VendorItemData const* items = vendor->GetVendorItems();
        if (!items)
        {
            return 0;
        }

        // Best candidate per equip slot first: a vendor stocking two
        // usable 1H weapons would otherwise sell us both for one hand.
        std::map<uint8, ItemTemplate const*> bestPerSlot;
        for (uint32_t i = 0; i < items->GetItemCount(); ++i)
        {
            VendorItem const* vendorItem = items->GetItem(i);
            if (!vendorItem || vendorItem->ExtendedCost)
            {
                continue;
            }
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            if (!proto || (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR))
            {
                continue;
            }
            // Real proficiency/level/skill gate -- the same check the
            // vendor UI greys items with (class weapon skills, armor
            // type, RequiredLevel).
            if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            {
                continue;
            }
            uint8 slot = bot->FindEquipSlot(proto, NULL_SLOT, true);
            if (slot == NULL_SLOT)
            {
                continue;
            }
            Item const* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (equipped && equipped->GetTemplate()->ItemLevel >= proto->ItemLevel)
            {
                continue;
            }
            auto it = bestPerSlot.find(slot);
            if (it == bestPerSlot.end() || it->second->ItemLevel < proto->ItemLevel)
            {
                bestPerSlot[slot] = proto;
            }
        }

        uint32_t bought = 0;
        for (auto const& [slot, proto] : bestPerSlot)
        {
            // Keep a repair reserve -- the durability death spiral is
            // worse than a missing upgrade (KNOWN_FAILURES #31).
            if (bot->GetMoney() < proto->BuyPrice + 200)
            {
                continue;
            }
            if (BuyItem(bot, vendor, proto->ItemId, 1))
            {
                ++bought;
            }
        }
        return bought;
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

        // The hearthstone and any other protected utility items must
        // survive vendoring (recovery depends on the hearthstone).
        constexpr uint32 HearthstoneItemId = 6948;

        bool IsSellableGray(Player* bot, Item const* item)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->SellPrice == 0)
            {
                return false;   // no vendor value / not sellable
            }

            // ---- never-sell protections (order matters) ----
            if (item->GetEntry() == HearthstoneItemId)
            {
                return false;   // protected utility item
            }
            if (proto->Class == ITEM_CLASS_CONTAINER)
            {
                return false;   // bags -- EquipBagUpgrades wants these
            }
            if (proto->Class == ITEM_CLASS_QUEST)
            {
                return false;   // quest items
            }
            if (proto->Class == ITEM_CLASS_PROJECTILE
                || proto->Class == ITEM_CLASS_QUIVER)
            {
                return false;   // ammo / quiver -- classes that need them
            }
            if (bot && bot->HasQuestForItem(item->GetEntry()))
            {
                return false;   // required for an ACTIVE quest objective
                                // (e.g. white drops for a collection quest)
            }
            if (proto->Class == ITEM_CLASS_CONSUMABLE
                && proto->SubClass == ITEM_SUBCLASS_FOOD)
            {
                return false;   // food/drink reserve
            }

            // ---- sellable ----
            if (proto->Quality == ITEM_QUALITY_POOR)
            {
                return true;    // all gray
            }
            // White/common gear + trade goods + non-food consumables.
            // After EquipBagUpgrades has taken any upgrade, leftover
            // white armor/weapons are vendor trash that otherwise pin
            // the bags (live: a bot sat 16/16 with 12 white armor
            // pieces; selljunk freed nothing and it "sold junk" every
            // 3 min forever). Equipped gear is never iterated here.
            if (proto->Quality == ITEM_QUALITY_NORMAL
                && (proto->Class == ITEM_CLASS_TRADE_GOODS
                    || proto->Class == ITEM_CLASS_CONSUMABLE
                    || proto->Class == ITEM_CLASS_ARMOR
                    || proto->Class == ITEM_CLASS_WEAPON))
            {
                return true;
            }
            return false;
        }
    } // namespace

    uint32_t CountSellableGrayItems(Player* bot)
    {
        if (!bot)
        {
            return 0;
        }

        uint32_t count = 0;
        ForEachCarriedItem(bot, [bot, &count](Item* item)
        {
            if (IsSellableGray(bot, item))
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

    uint32_t CountFoodDrinkConsumables(Player* bot)
    {
        if (!bot)
        {
            return 0;
        }

        uint32_t count = 0;
        ForEachCarriedItem(bot, [&count](Item* item)
        {
            ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
            if (proto && proto->Class == ITEM_CLASS_CONSUMABLE
                && proto->SubClass == ITEM_SUBCLASS_FOOD)
            {
                count += item->GetCount();
            }
        });
        return count;
    }

    namespace
    {
        // Food restores health (SPELL_AURA_MOD_REGEN), drink restores
        // mana (SPELL_AURA_OBS_MOD_POWER) -- both live under
        // ITEM_SUBCLASS_FOOD in 3.3.5, so the use-spell's aura is the
        // only reliable discriminator.
        bool IsFoodOrDrink(ItemTemplate const* proto, bool& isDrink)
        {
            if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE
                || proto->SubClass != ITEM_SUBCLASS_FOOD)
            {
                return false;
            }
            for (_Spell const& spell : proto->Spells)
            {
                if (spell.SpellId <= 0)
                {
                    continue;
                }
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId);
                if (!info)
                {
                    continue;
                }
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                {
                    if (info->Effects[i].ApplyAuraName == SPELL_AURA_OBS_MOD_POWER)
                    {
                        isDrink = true;
                        return true;
                    }
                    if (info->Effects[i].ApplyAuraName == SPELL_AURA_MOD_REGEN
                        || info->Effects[i].ApplyAuraName == SPELL_AURA_OBS_MOD_HEALTH)
                    {
                        isDrink = false;
                        return true;
                    }
                }
            }
            return false;
        }
    }

    uint32_t BuyConsumables(Player* bot, Creature* vendor, uint32_t wantEach)
    {
        if (!bot || !bot->GetSession() || !vendor)
        {
            return 0;
        }
        VendorItemData const* items = vendor->GetVendorItems();
        if (!items)
        {
            return 0;
        }

        uint32_t haveFood = 0, haveDrink = 0;
        ForEachCarriedItem(bot, [&haveFood, &haveDrink](Item* item)
        {
            bool drink = false;
            if (item && IsFoodOrDrink(item->GetTemplate(), drink))
            {
                (drink ? haveDrink : haveFood) += item->GetCount();
            }
        });

        ItemTemplate const* bestFood = nullptr;
        ItemTemplate const* bestDrink = nullptr;
        for (uint32_t i = 0; i < items->GetItemCount(); ++i)
        {
            VendorItem const* vendorItem = items->GetItem(i);
            if (!vendorItem || vendorItem->ExtendedCost)
            {
                continue;
            }
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vendorItem->item);
            bool drink = false;
            if (!proto || !IsFoodOrDrink(proto, drink)
                || proto->RequiredLevel > bot->GetLevel())
            {
                continue;
            }
            ItemTemplate const*& best = drink ? bestDrink : bestFood;
            if (!best || best->ItemLevel < proto->ItemLevel)
            {
                best = proto;
            }
        }

        uint32_t bought = 0;
        for (auto const& [best, have] : {std::pair{bestFood, haveFood},
                                         std::pair{bestDrink, haveDrink}})
        {
            if (!best || have >= wantEach)
            {
                continue;
            }
            uint32_t need = wantEach - have;
            uint32_t stack = std::max<uint32_t>(1, best->BuyCount);
            uint32_t buys = (need + stack - 1) / stack;
            for (uint32_t n = 0; n < buys; ++n)
            {
                if (bot->GetMoney() < best->BuyPrice + 200)
                {
                    break;
                }
                if (BuyItem(bot, vendor, best->ItemId, stack))
                {
                    ++bought;
                }
            }
        }
        return bought;
    }

    bool UseFoodDrink(Player* bot, bool preferDrink)
    {
        if (!bot || !bot->GetSession())
        {
            return false;
        }
        Item* pick = nullptr;
        bool pickIsDrink = false;
        ForEachCarriedItem(bot, [&pick, &pickIsDrink, preferDrink](Item* item)
        {
            bool drink = false;
            if (!item || item->IsInTrade() || !IsFoodOrDrink(item->GetTemplate(), drink))
            {
                return;
            }
            if (!pick || (preferDrink && drink && !pickIsDrink)
                || (!preferDrink && !drink && pickIsDrink))
            {
                pick = item;
                pickIsDrink = drink;
            }
        });
        if (!pick)
        {
            return false;
        }
        // Real self-targeted item use (eating/drinking sits the bot via
        // the spell itself); movement cancels it, which is the caller's
        // concern (only use while idle).
        uint32_t spellId = 0;
        for (_Spell const& spell : pick->GetTemplate()->Spells)
        {
            if (spell.SpellId > 0)
            {
                spellId = spell.SpellId;
                break;
            }
        }
        if (!spellId || pick->IsEquipped())
        {
            return false;
        }
        bot->StopMoving();
        WorldPacket packet(CMSG_USE_ITEM);
        packet << uint8_t(pick->GetBagSlot()) << uint8_t(pick->GetSlot())
               << uint8_t(0) << spellId << pick->GetGUID() << uint32_t(0)
               << uint8_t(0) << uint32_t(0);  // TARGET_FLAG_NONE = self
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
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
        ForEachCarriedItem(bot, [bot, &toSell](Item* item)
        {
            if (IsSellableGray(bot, item))
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
