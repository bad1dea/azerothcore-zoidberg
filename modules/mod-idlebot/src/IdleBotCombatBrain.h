#ifndef MOD_IDLEBOT_COMBATBRAIN_H
#define MOD_IDLEBOT_COMBATBRAIN_H

// IdleBotCombatBrain
// =============================================================================
// Questing-combat intelligence layer: danger evaluation, pull decisions, kill
// classification, path/area clearing logic. Stateless pure analysis — no bridge
// calls, no DB writes. IdleBotManager reads context and calls these helpers;
// it owns all bridge interaction.
//
// Design rules:
// - All methods are const and take only inputs → unit-testable off-server.
// - No raw pointers to live objects; callers pass values/refs.
// - Use TargetRole for kill telemetry so logs are greppable per category.

#include "IdleBotPlayerbotBridge.h"
#include <cstdint>
#include <cmath>
#include <string>

namespace idlebot
{
    // -------------------------------------------------------------------------
    // DangerLevel — output of the danger evaluator.
    // Safe        → proceed with pull.
    // Risky       → can engage but back-off if hp drops.
    // Avoid       → too many hostiles / hp too low; don't engage, roam away.
    // ClearFirst  → a specific threat must be handled before objective approach.
    // Retreat     → disengage immediately (critical hp or overwhelmed).
    // Pause       → death circuit breaker is active; stop all engagement.
    enum class DangerLevel : uint8_t
    {
        Safe,
        Risky,
        Avoid,
        ClearFirst,
        Retreat,
        Pause
    };

    // TargetRole — why we are (or were) fighting a creature.
    // Used for kill classification telemetry.
    enum class TargetRole : uint8_t
    {
        ObjectiveTarget,  // matches step.creatureIds → direct quest kill
        DefensiveAdd,     // attacked us while we were targeting something else
        PathBlocker,      // between bot and objective; cleared to make passage
        AccidentalPull,   // proximity / AoE pull, not intentional
        Invalid           // over-level, blacklisted, tapped, etc.
    };

    inline char const* ToString(DangerLevel lvl)
    {
        switch (lvl)
        {
            case DangerLevel::Safe:       return "safe";
            case DangerLevel::Risky:      return "risky";
            case DangerLevel::Avoid:      return "avoid";
            case DangerLevel::ClearFirst: return "clear_first";
            case DangerLevel::Retreat:    return "retreat";
            case DangerLevel::Pause:      return "pause";
        }
        return "unknown";
    }

    inline char const* ToString(TargetRole role)
    {
        switch (role)
        {
            case TargetRole::ObjectiveTarget: return "objective_target";
            case TargetRole::DefensiveAdd:    return "defensive_add";
            case TargetRole::PathBlocker:     return "path_blocker";
            case TargetRole::AccidentalPull:  return "accidental_pull";
            case TargetRole::Invalid:         return "invalid";
        }
        return "unknown";
    }

    // -------------------------------------------------------------------------
    // DangerAssessment — output of IdleBotDangerEvaluator::Evaluate().
    struct DangerAssessment
    {
        DangerLevel level = DangerLevel::Safe;
        char const* reason = "";
    };

    // -------------------------------------------------------------------------
    // IdleBotDangerEvaluator — stateless danger assessment.
    // Call Evaluate() each ENGAGE tick before committing to a pull.
    class IdleBotDangerEvaluator
    {
    public:
        struct Config
        {
            uint32_t maxPull = 3;           // bot's configured max simultaneous attackers
            uint32_t criticalHpPct = 25;    // retreat threshold
            uint32_t lowHpPct = 35;         // recover threshold
            uint32_t lowManaPct = 20;       // mana recover threshold
            uint32_t packAvoidSize = 5;     // don't initiate pull if aoeCount >= this
        };

        // Assess danger for the current ENGAGE frame.
        // cc       — live combat context from GetCombatContext()
        // cfg      — manager config
        // paused   — death circuit breaker active
        DangerAssessment Evaluate(CombatContext const& cc, Config const& cfg, bool paused) const;
    };

    // -------------------------------------------------------------------------
    // IdleBotPullManager — stateless pull decision helpers.
    class IdleBotPullManager
    {
    public:
        // Can we pull this specific target right now?
        // Returns false if level overcap, blacklisted, blackspotted, or at maxPull.
        // rec  — current BotRecord (read-only; blacklist/blackspot checks)
        // targetLevel  — creature's level (0 = unknown; treated as pullable)
        // botLevel     — bot's current level
        // targetGuid   — creature's GUID
        // targetPos    — creature's world position
        // maxPull      — bot's configured pull cap
        // levelOvercap — max levels above bot we'll fight (default 5)
        template<typename BotRecordT>
        static bool CanPull(
            CombatContext const& cc,
            uint32_t targetLevel,
            uint32_t botLevel,
            uint64_t targetGuid,
            BotPosition const& targetPos,
            BotRecordT const& rec,
            uint32_t maxPull,
            uint32_t levelOvercap = 5)
        {
            // Already at pull cap
            if (cc.myAttackers >= maxPull)
                return false;

            // Level filter: skip mobs significantly over-level
            if (targetLevel > 0 && botLevel > 0 && targetLevel > botLevel + levelOvercap)
                return false;

            // Target blacklist (guid → expiry tick)
            {
                auto bit = rec.targetBlacklist.find(targetGuid);
                if (bit != rec.targetBlacklist.end() && rec.globalTick < bit->second)
                    return false;
            }

            // Blackspot avoidance (stuck position markers)
            if (targetPos.valid && !rec.blackspots.empty())
            {
                for (auto const& bs : rec.blackspots)
                {
                    float bx = targetPos.x - bs.first;
                    float by = targetPos.y - bs.second;
                    if ((bx * bx + by * by) < 10.f * 10.f)
                        return false;
                }
            }

            return true;
        }

        // Is this target the guide-specified quest creature?
        static bool IsObjectiveTarget(uint64_t targetGuid, uint64_t questTargetGuid)
        {
            return targetGuid != 0 && targetGuid == questTargetGuid;
        }

        // Is this target physically between the bot and the objective?
        // "Between" = target is closer to objective than bot is, AND within clearRadius
        // of the straight-line path from bot to objective.
        static bool IsPathBlocker(
            BotPosition const& botPos,
            BotPosition const& targetPos,
            BotPosition const& objectivePos,
            float clearRadius)
        {
            if (!botPos.valid || !targetPos.valid || !objectivePos.valid)
                return false;
            if (botPos.mapId != targetPos.mapId)
                return false;

            float const pathDx = objectivePos.x - botPos.x;
            float const pathDy = objectivePos.y - botPos.y;
            float const pathLen = std::hypot(pathDx, pathDy);
            if (pathLen < 1.f)
                return false;

            // Signed distance along path direction (positive = toward objective)
            float const tx = targetPos.x - botPos.x;
            float const ty = targetPos.y - botPos.y;
            float const along = (tx * pathDx + ty * pathDy) / pathLen;
            if (along <= 0.f || along > pathLen)
                return false;   // behind us or beyond objective

            // Perpendicular distance from path line
            float const cross = (tx * pathDy - ty * pathDx) / pathLen;
            return std::fabs(cross) <= clearRadius;
        }

        // Classify a target's role given the context.
        // questTargetGuid — the guid returned by FindNearestQuestCreature (0 = none)
        // wasIntentionalAttack — did IdleBot explicitly choose to attack this target?
        static TargetRole Classify(
            uint64_t targetGuid,
            uint64_t questTargetGuid,
            bool wasIntentionalPathClear,
            bool wasIntentionalAttack)
        {
            if (targetGuid == 0)
                return TargetRole::Invalid;
            if (targetGuid == questTargetGuid)
                return TargetRole::ObjectiveTarget;
            if (wasIntentionalPathClear)
                return TargetRole::PathBlocker;
            if (wasIntentionalAttack)
                return TargetRole::DefensiveAdd;
            return TargetRole::AccidentalPull;
        }
    };

    // -------------------------------------------------------------------------
    // KillStats — per-bot kill classification counters.
    // Lives in BotRecord; reset on guide change.
    struct KillStats
    {
        uint32_t objectiveTarget = 0;   // guide-specified quest creature killed
        uint32_t defensiveAdd = 0;      // attacked us; we killed it
        uint32_t pathBlocker = 0;       // cleared from path to objective
        uint32_t accidental = 0;        // unintentional pull
    };

} // namespace idlebot

#endif // MOD_IDLEBOT_COMBATBRAIN_H
