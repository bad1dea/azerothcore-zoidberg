#include "BotQuestBehaviors.h"

#include "ConditionMgr.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::QuestBehaviors
{
    namespace
    {
        uint32_t FirstUseSpell(ItemTemplate const* proto)
        {
            if (!proto)
                return 0;
            for (_Spell const& spell : proto->Spells)
                if (spell.SpellId > 0)
                    return spell.SpellId;
            return 0;
        }

        uint32_t FirstUseSpell(Item const* item)
        {
            return item ? FirstUseSpell(item->GetTemplate()) : 0;
        }

        bool WriteItemUsePrefix(Player* bot, Item* item, WorldPacket& packet)
        {
            uint32_t const spellId = FirstUseSpell(item);
            if (!bot || !bot->GetSession() || !item || !spellId || item->IsEquipped())
                return false;
            packet << uint8_t(item->GetBagSlot()) << uint8_t(item->GetSlot()) << uint8_t(0)
                   << spellId << item->GetGUID() << uint32_t(0) << uint8_t(0);
            return true;
        }
    }

    uint32_t QuestProgress(Player const* bot, uint32_t questId)
    {
        if (!bot || !questId)
            return 0;
        uint32_t progress = 0;
        for (uint16_t slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        {
            if (bot->GetQuestSlotQuestId(slot) != questId)
                continue;
            for (uint8_t objective = 0; objective < QUEST_OBJECTIVES_COUNT; ++objective)
                progress += bot->GetQuestSlotCounter(slot, objective);
            break;
        }
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            for (uint32_t itemId : quest->RequiredItemId)
                if (itemId)
                    progress += bot->GetItemCount(itemId, true);
        return progress;
    }

    bool RequestGameObjectUse(Player* bot, ObjectGuid const& guid)
    {
        if (!bot || !bot->GetSession() || guid.IsEmpty())
            return false;
        WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
        packet << guid;
        bot->GetSession()->HandleGameObjectUseOpcode(packet);
        return true;
    }

    bool UnitMeetsItemUseConditions(Player* bot, uint32_t itemId, Unit* target)
    {
        if (!bot || !target)
            return false;
        uint32_t const spellId = FirstUseSpell(sObjectMgr->GetItemTemplate(itemId));
        if (!spellId)
            return true;
        // Some quest items' use-spells carry world-DB target conditions
        // (e.g. Foreman's Blackjack 16114 -> Awaken Peon 19938 requires
        // the Peon Sleep aura 17743); casting on a non-qualifying unit
        // fails silently with no quest credit.
        ConditionList const conditions =
            sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, spellId);
        if (conditions.empty())
            return true;
        ConditionSourceInfo sourceInfo(bot, target);
        return sConditionMgr->IsObjectMeetToConditions(sourceInfo, conditions);
    }

    bool RequestItemUseOnUnit(Player* bot, uint32_t itemId, ObjectGuid const& guid)
    {
        Item* item = bot ? bot->GetItemByEntry(itemId) : nullptr;
        WorldPacket packet(CMSG_USE_ITEM);
        if (!WriteItemUsePrefix(bot, item, packet) || guid.IsEmpty())
            return false;
        packet << uint32_t(TARGET_FLAG_UNIT) << guid.WriteAsPacked();
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
    }

    bool RequestItemUseAtLocation(Player* bot, uint32_t itemId, float x, float y, float z)
    {
        Item* item = bot ? bot->GetItemByEntry(itemId) : nullptr;
        WorldPacket packet(CMSG_USE_ITEM);
        if (!WriteItemUsePrefix(bot, item, packet))
            return false;
        packet << uint32_t(TARGET_FLAG_DEST_LOCATION);
        packet.appendPackGUID(0);
        packet << x << y << z;
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
    }

    bool RequestAreaTrigger(Player* bot, uint32_t areaTriggerId)
    {
        if (!bot || !bot->GetSession() || !sObjectMgr->GetAreaTrigger(areaTriggerId))
            return false;
        WorldPacket packet(CMSG_AREATRIGGER, 4);
        packet << areaTriggerId;
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        return true;
    }
}
