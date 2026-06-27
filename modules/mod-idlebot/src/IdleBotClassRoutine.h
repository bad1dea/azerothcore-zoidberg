#ifndef MOD_IDLEBOT_CLASSROUTINE_H
#define MOD_IDLEBOT_CLASSROUTINE_H

// IdleBotClassRoutine
// =============================================================================
// Per-class strategy configuration layer. Each class knows what positioning
// and strategy expressions to set on the playerbots AI, and what per-class
// combat config overrides (aoe threshold, mana rest threshold) to apply.
//
// Scope: IdleBot controls STRATEGY SELECTION — which playerbots strategy
// expressions to activate (+ranged, +close, +aoe, etc.) and when. The actual
// spell rotation stays inside playerbots. We only choose the mode/stance.
//
// Known-good strategy expressions (confirmed working):
//   "+ranged", "+close", "+aoe", "-aoe", "+loot", "buff"
//
// Class IDs (WoW 3.3.5):
//   1=Warrior, 2=Paladin, 3=Hunter, 4=Rogue, 5=Priest
//   6=Shaman, 7=Mage, 8=Warlock, 9=Druid

#include "IdleBotPlayerbotBridge.h"
#include <cstdint>
#include <memory>

namespace idlebot
{
    // Per-class combat parameter overrides. IdleBotManager reads these
    // and substitutes them for the global config when _classRoutine is set.
    struct ClassRoutineConfig
    {
        uint32_t aoeThreshold = 3;            // switch +aoe when cluster >= this
        uint32_t restBeforePullManaPct = 50;  // class-specific mana floor before pull
        uint32_t restBeforePullHpPct = 70;    // class-specific hp floor before pull
        bool skipManaRecover = false;         // warrior: no mana; skip Recover() for mana
        bool aggressiveDrink = false;         // mage/priest: drink at higher mana threshold
        uint32_t maxPullOverride = 0;         // 0 = use global; otherwise class-specific cap
    };

    class IdleBotClassRoutine
    {
    public:
        virtual ~IdleBotClassRoutine() = default;

        // Set playerbots strategies appropriate for this class + current context.
        // Called once per session in EnsureStrategies (strategiesEnsured gate).
        virtual void ConfigureStrategies(
            IdleBotPlayerbotBridge* bridge,
            BotGuid bot,
            CombatContext const& cc) const = 0;

        // Per-class combat parameter overrides for IdleBotManager to apply.
        virtual ClassRoutineConfig GetConfig() const = 0;

        // Short class name for logging.
        virtual char const* ClassName() const = 0;

        // Factory: returns a concrete routine for the given WoW classId, or
        // a default pass-through routine if the classId is unrecognized.
        static std::unique_ptr<IdleBotClassRoutine> Make(uint8_t classId);
    };

} // namespace idlebot

#endif // MOD_IDLEBOT_CLASSROUTINE_H
