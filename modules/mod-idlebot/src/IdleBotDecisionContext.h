#ifndef MOD_IDLEBOT_DECISIONCONTEXT_H
#define MOD_IDLEBOT_DECISIONCONTEXT_H

#include "IdleBotGuide.h"
#include "IdleBotPlayerbotBridge.h"
#include <cstdint>

namespace idlebot
{
    // Everything the decision engine reads to choose the next action. Assembled
    // fresh each tick by the manager/executor from bridge reads + failure tracker.
    // Pure data; no logic.
    struct DecisionContext
    {
        // who
        BotGuid bot = 0;
        uint32_t level = 0;
        uint32_t classId = 0;

        // where the guide says to be
        const GuideStep* currentStep = nullptr;
        bool stepRequiredForChain = true;

        // live world/character state
        BotPosition position;
        InventoryStatus inventory;
        bool isDead = false;

        // failure history (filled by IdleBotFailureTracker)
        uint32_t deathsInCurrentArea = 0;
        uint32_t stuckEventsRecent = 0;
        uint32_t secondsSinceProgress = 0;
        uint32_t minutesOnCurrentStep = 0;

        // surroundings (M7+/M8+)
        uint32_t nearbyPlayerCount = 0;
        bool nearbyPlayerSameObjective = false;
        bool inGroup = false;
    };
}

#endif // MOD_IDLEBOT_DECISIONCONTEXT_H
