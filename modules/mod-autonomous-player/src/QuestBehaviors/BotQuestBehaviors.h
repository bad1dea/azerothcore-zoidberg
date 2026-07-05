#ifndef AUTONOMOUS_PLAYER_BOT_QUEST_BEHAVIORS_H
#define AUTONOMOUS_PLAYER_BOT_QUEST_BEHAVIORS_H

#include "Define.h"
#include "ObjectGuid.h"

class Player;
class Unit;

namespace AutonomousPlayer::QuestBehaviors
{
    uint32_t QuestProgress(Player const* bot, uint32_t questId);
    bool RequestGameObjectUse(Player* bot, ObjectGuid const& guid);
    bool UnitMeetsItemUseConditions(Player* bot, uint32_t itemId, Unit* target);
    bool RequestItemUseOnUnit(Player* bot, uint32_t itemId, ObjectGuid const& guid);
    bool RequestItemUseAtLocation(Player* bot, uint32_t itemId, float x, float y, float z);
    bool RequestAreaTrigger(Player* bot, uint32_t areaTriggerId);
}

#endif
