#ifndef MOD_IDLEBOT_GUIDE_H
#define MOD_IDLEBOT_GUIDE_H

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace idlebot
{
    // Step types supported by the guide format. Mirror data/guides/*.yaml.
    enum class StepType
    {
        MoveTo,
        AcceptQuest,
        TurnInQuest,
        KillMobs,
        LootItems,
        InteractGameobject,
        UseItemOnNpc,        // use a quest item on a creature (CAST quests, SpecialFlags=32)
        TalkToNpc,
        TrainClassSkills,
        Vendor,
        Repair,
        EquipUpgrade,
        SetHearthstone,
        UseHearthstone,
        GrindUntilLevel,
        DiscoverFlightPath,
        EscortQuest,         // follow + defend an NPC until quest completes
        TaxiRide,            // take a flight path to a destination
        GossipInteract,      // interact with NPC choosing a specific gossip option
        Conditional,
        Checkpoint,
        Fallback,
        Unknown
    };

    struct Coordinates
    {
        uint32_t mapId = 0;
        float x = 0.f, y = 0.f, z = 0.f;
        float radius = 5.f;
        bool isTodoPlaceholder = false;   // true => coords are guessed, mark in logs
    };

    // Adaptive metadata (decision engine, M7+). Ignored by strict mode.
    struct AdaptiveMeta
    {
        bool optional = false;
        bool skippable = false;
        bool requiredForChain = true;
        uint32_t maxAttemptMinutes = 0;     // 0 = use global default
        uint32_t maxDeaths = 0;
        bool allowAlternateAreas = false;
        bool allowGrouping = false;
        bool allowGrindFallback = false;
        std::vector<std::string> fallbackSteps;
        // alternate_objectives / failure_policy parsed into richer structs later.
    };

    struct GuideStep
    {
        std::string id;
        std::string name;
        StepType type = StepType::Unknown;

        uint32_t levelMin = 0;
        uint32_t levelMax = 0;

        // requirements (optional)
        std::optional<uint32_t> raceMask;
        std::optional<uint32_t> classMask;
        std::optional<uint32_t> factionMask;

        // targets (any subset relevant to the step type)
        std::optional<uint32_t> questId;
        std::optional<uint32_t> npcId;
        std::optional<uint32_t> gameobjectId;
        std::optional<uint32_t> itemId;
        std::optional<uint32_t> gossipOption;  // gossip menu option index (0-based)
        std::optional<uint32_t> taxiNodeId;    // destination taxi node for TaxiRide
        std::vector<uint32_t> creatureIds;

        Coordinates coords;
        std::vector<Coordinates> hotspots;  // patrol waypoints for kill objectives

        // Nearest vendor NPC for vendor runs (embedded by generate_vendor_coords.py).
        std::optional<uint32_t> vendorEntry;
        Coordinates vendorCoords;

        std::string completionCondition;    // free-form, interpreted by executor
        uint32_t timeoutSeconds = 0;
        uint32_t retryCount = 0;

        AdaptiveMeta adaptive;
        std::string notes;
    };

    struct Guide
    {
        std::string id;            // e.g. "tauren-camp_narache-1-6"
        std::string name;
        std::string faction;       // alliance / horde
        std::string race;
        std::string klass;         // "class" is reserved
        uint32_t levelMin = 1;
        uint32_t levelMax = 6;
        std::string nextGuide;     // chain: auto-load this guide when done
        std::vector<GuideStep> steps;

        bool Valid(std::string& outErr) const;   // basic structural validation
    };
}

#endif // MOD_IDLEBOT_GUIDE_H
