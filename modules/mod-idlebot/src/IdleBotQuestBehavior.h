#ifndef MOD_IDLEBOT_QUESTBEHAVIOR_H
#define MOD_IDLEBOT_QUESTBEHAVIOR_H

// IdleBotQuestBehavior
// =============================================================================
// Base class for modular, reusable quest behavior implementations.
// Inspired by Honorbuddy's QuestBehaviorBase pattern: each behavior is a
// self-contained state machine with a defined lifecycle and completion check.
//
// Current behaviors implemented:
//   CollectItems   — collect items from GOs/creatures until count reached
//                    (StepType::CollectItems in IdleBotManager)
//   InteractWith   — interact with NPC/GO; gossip/quest-frame/loot/item-use
//                    (StepType::InteractGameobject, TalkToNpc, GossipInteract)
//
// Future behaviors (not yet implemented):
//   UseItemOn           — use quest item on NPC/GO/location
//   KillUntilComplete   — kill mobs until objective complete; handles respawn
//   Escort              — follow + protect NPC until quest objective completes
//   TaxiRide            — board/ride/disembark transport node
//   UseTransport        — generic transport (boat/zeppelin/tram)
//   SetHearthstone      — bind hearthstone at an inn
//   WaitTimer           — explicit timed wait with suppressed unstick
//
// Design rules (DO NOT VIOLATE):
// - No blocking in Tick(). Do ONE small action per call, return immediately.
// - No DB writes in Tick(). All heavy work is offline/pre-generated.
// - Store ObjectGuid, not raw pointers. Pointers dangle; GUIDs are stable.
// - Blacklist bad targets locally; never mark them globally.
// - Log with QB_<BEHAVIOR> prefix so logs are greppable per behavior type.

#include <cstdint>
#include <string>

namespace idlebot
{
    enum class BehaviorStatus
    {
        Running,   // in progress, call Tick() next frame
        Success,   // objective reached; manager may advance to next step
        Failed,    // unrecoverable failure; manager should skip or abort
        Skipped,   // objective was optional and timed out; treat as success
        Waiting,   // intentionally blocked (respawn/target/cooldown); suppress unstick
        Blocked    // required target unavailable and timeout exceeded; log and wait
    };

    // Compact reason strings for structured logs and telemetry.
    // Use these constants rather than ad-hoc strings so grep works.
    namespace FailReason
    {
        inline constexpr char const* NoSources          = "no_sources";
        inline constexpr char const* NoSpawnedSources   = "no_spawned_sources";
        inline constexpr char const* ItemCountReached   = "item_count_reached";
        inline constexpr char const* QuestObjectiveMet  = "quest_objective_met";
        inline constexpr char const* Timeout            = "timeout";
        inline constexpr char const* OptionalTimeout    = "optional_timeout";
        inline constexpr char const* RequiredWait       = "required_wait";
        inline constexpr char const* MaxAttemptsReached = "max_attempts";
        inline constexpr char const* NoItemAfterInteract = "no_item_after_interact";
        inline constexpr char const* Misconfigured      = "misconfigured";
    }
}

#endif // MOD_IDLEBOT_QUESTBEHAVIOR_H
