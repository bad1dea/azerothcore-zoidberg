#ifndef MOD_IDLEBOT_DECISIONENGINE_H
#define MOD_IDLEBOT_DECISIONENGINE_H

#include "IdleBotDecision.h"
#include "IdleBotDecisionContext.h"
#include <string>

namespace idlebot
{
    // Decision modes. Wider modes permit more deviation from the guide.
    enum class DecisionMode
    {
        StrictGuide,        // follow guide; deviate only for death/stuck/full bags/broken state
        GuideAssisted,      // allow alternate objectives, grinding, vendor/trainer, safe skips
        AutonomousLeveling, // guide is a suggestion; freely choose quests/grind/group
        SandboxIdle         // bot picks its own activities from allowed goals
    };

    DecisionMode ParseMode(const std::string& s);
    const char* ToString(DecisionMode m);

    // IdleBotDecisionEngine
    // -------------------------------------------------------------------------
    // Sits between guide planner and step executor. Given context, returns the
    // best next action. M1–M6: this is a STUB that always returns
    // ContinueCurrentStep (strict behavior). Real scoring arrives in M7.
    //
    // Keep this pure: it reads DecisionContext, returns a Decision. No bridge
    // calls, no DB writes, no blocking. That makes it unit-testable off-server.
    class IdleBotDecisionEngine
    {
    public:
        explicit IdleBotDecisionEngine(DecisionMode mode) : _mode(mode) {}

        Decision Decide(const DecisionContext& ctx) const;

        void SetMode(DecisionMode m) { _mode = m; }
        DecisionMode GetMode() const { return _mode; }

    private:
        // M7+: each of these returns a scored candidate; Decide() picks the max.
        // float ScoreContinue(const DecisionContext&) const;
        // float ScoreGrind(const DecisionContext&) const;
        // float ScoreVendorRepair(const DecisionContext&) const;
        // ... etc per the scoring inputs in DECISION_ENGINE.md

        DecisionMode _mode = DecisionMode::StrictGuide;
    };
}

#endif // MOD_IDLEBOT_DECISIONENGINE_H
