#ifndef MOD_IDLEBOT_DECISION_H
#define MOD_IDLEBOT_DECISION_H

#include <string>

namespace idlebot
{
    // The set of high-level actions the decision engine can choose. The executor
    // translates the chosen one into bridge calls.
    enum class DecisionAction
    {
        ContinueCurrentStep,
        RetryWithAlternateArea,
        GrindUntilLevel,
        VendorRepairTrain,
        SkipOptionalStep,
        MarkQuestBlocked,
        AskForGroup,
        AcceptGroupInvite,
        LeaveGroup,
        ReturnToTown,
        PauseForManualReview,
        SwitchToFallbackGuideBranch
    };

    struct Decision
    {
        DecisionAction action = DecisionAction::ContinueCurrentStep;
        float score = 0.f;          // winning score, for telemetry
        std::string reason;         // human-readable, logged to idlebot_decision_history
    };

    const char* ToString(DecisionAction a);
}

#endif // MOD_IDLEBOT_DECISION_H
