#include "IdleBotCombatBrain.h"

namespace idlebot
{
    DangerAssessment IdleBotDangerEvaluator::Evaluate(
        CombatContext const& cc,
        Config const& cfg,
        bool paused) const
    {
        if (paused)
            return { DangerLevel::Pause, "circuit_breaker_active" };

        if (!cc.valid)
            return { DangerLevel::Safe, "no_context" };

        // Already overwhelmed → retreat (mirrors RETREAT mode trigger)
        if (cc.hpPct < static_cast<float>(cfg.criticalHpPct))
            return { DangerLevel::Retreat, "critical_hp" };

        if (cc.hpPct < 40.f && cc.aoeCount >= 4)
            return { DangerLevel::Retreat, "swarmed" };

        // Need to recover (mirrors RECOVER mode trigger)
        if (!cc.inCombat &&
            (cc.hpPct < static_cast<float>(cfg.lowHpPct) ||
             cc.manaPct < static_cast<float>(cfg.lowManaPct)))
            return { DangerLevel::Avoid, "low_resources" };

        // Pack too large to safely enter: more mobs clustered here than we can handle.
        // This only applies BEFORE engagement — once in combat we commit.
        if (!cc.inCombat && cc.aoeCount >= static_cast<uint32_t>(cfg.packAvoidSize))
            return { DangerLevel::ClearFirst, "large_pack" };

        // Already have enough attackers — don't add more
        if (!cc.inCombat && cc.myAttackers >= cfg.maxPull)
            return { DangerLevel::Avoid, "at_pull_cap" };

        if (cc.aoeCount >= cfg.maxPull)
            return { DangerLevel::Risky, "near_aoe_threshold" };

        return { DangerLevel::Safe, "ok" };
    }

} // namespace idlebot
