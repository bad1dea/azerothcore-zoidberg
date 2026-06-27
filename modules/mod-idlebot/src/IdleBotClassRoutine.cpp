#include "IdleBotClassRoutine.h"

namespace idlebot
{
    namespace
    {
        // Default pass-through: cc.ranged heuristic, global config unchanged.
        struct DefaultRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "unknown"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& cc) const override
            {
                bridge->SetCombatStrategy(bot, cc.ranged ? "+ranged" : "+close");
            }

            ClassRoutineConfig GetConfig() const override { return {}; }
        };

        // ---- 1: Warrior ----
        // Melee only, no mana. Protect at low levels by capping pull size and
        // requiring more HP before the next pull (no self-heal, no food shortcut).
        struct WarriorRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "warrior"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+close");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.skipManaRecover = true;    // no mana — never call Recover() for mana
                cfg.restBeforePullHpPct = 80;  // needs more HP between pulls (can't heal)
                cfg.maxPullOverride = 2;       // low-level warrior dies to chain-pulls
                return cfg;
            }
        };

        // ---- 2: Paladin ----
        // Melee hybrid with heals and mana-heavy buffs. Needs mana for healing;
        // rest more aggressively than the default.
        struct PaladinRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "paladin"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+close");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.aggressiveDrink = true;
                cfg.restBeforePullManaPct = 60; // needs mana for Lay on Hands / heals
                return cfg;
            }
        };

        // ---- 3: Hunter ----
        // Ranged, pet tank. Pet handles adds; hunter can sustain longer chains.
        // AoE threshold lowered: Multi-Shot + Volley work at 2.
        struct HunterRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "hunter"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+ranged");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.aoeThreshold = 2; // Multi-Shot hits 2+ efficiently
                cfg.restBeforePullManaPct = 40; // focus regen; less mana-dependent
                return cfg;
            }
        };

        // ---- 4: Rogue ----
        // Melee, energy (not mana). Energy regens fast; shorter rest between pulls.
        // Can stealth past some packs (playerbots handles stealth decisions).
        struct RogueRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "rogue"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+close");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.skipManaRecover = true;      // energy, not mana
                cfg.restBeforePullHpPct = 65;    // energy regens; pull sooner
                cfg.restBeforePullManaPct = 0;   // energy bots ignore mana gate
                return cfg;
            }
        };

        // ---- 5: Priest ----
        // Cloth caster/healer. Wand DPS, shield absorb, very mana-hungry.
        // Must drink aggressively; AoE is weak at low level (Mind Sear is WotLK only).
        struct PriestRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "priest"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+ranged");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.aggressiveDrink = true;
                cfg.restBeforePullManaPct = 65; // needs lots of mana for heals + spells
                cfg.restBeforePullHpPct = 75;
                cfg.aoeThreshold = 4;           // priest AoE is limited until 40+
                cfg.maxPullOverride = 2;        // cloth armor; don't chain-pull
                return cfg;
            }
        };

        // ---- 6: Shaman ----
        // Melee hybrid with totems, Chain Lightning, Lava burst. Good AoE.
        struct ShamanRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "shaman"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                // Enhancement shaman fights melee; elemental fights ranged.
                // playerbots' cc.ranged already handles this — mirror it.
                bridge->SetCombatStrategy(bot, "+close");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.aoeThreshold = 2; // Chain Lightning / Earth Shock AoE works at 2
                cfg.aggressiveDrink = true;
                cfg.restBeforePullManaPct = 55;
                return cfg;
            }
        };

        // ---- 7: Mage ----
        // Pure ranged caster. Drinks between EVERY pull (mana = DPS = survival).
        // Excellent AoE (Arcane Explosion, Blizzard, Flamestrike, Frost Nova).
        struct MageRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "mage"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+ranged");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.aggressiveDrink = true;
                cfg.restBeforePullManaPct = 70; // must drink to full; mana = survival
                cfg.restBeforePullHpPct = 60;
                cfg.aoeThreshold = 2;           // AoE is mage's identity
                return cfg;
            }
        };

        // ---- 8: Warlock ----
        // Ranged with pet, drain life sustain, soul shards. Less mana-stressed
        // than mage due to drain life self-healing. AoE via Rain of Fire / Seed.
        struct WarlockRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "warlock"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+ranged");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.restBeforePullManaPct = 45; // drain life offsets mana use
                cfg.aoeThreshold = 3;
                return cfg;
            }
        };

        // ---- 9: Druid ----
        // Shapeshifter. Feral (bear/cat) at low levels; +close is appropriate.
        // Bear AoE (swipe) works at 2; cat is single-target energy like rogue.
        struct DruidRoutine final : IdleBotClassRoutine
        {
            char const* ClassName() const override { return "druid"; }

            void ConfigureStrategies(IdleBotPlayerbotBridge* bridge, BotGuid bot,
                CombatContext const& /*cc*/) const override
            {
                bridge->SetCombatStrategy(bot, "+close");
            }

            ClassRoutineConfig GetConfig() const override
            {
                ClassRoutineConfig cfg;
                cfg.aoeThreshold = 2;           // bear swipe AoE
                cfg.aggressiveDrink = true;
                cfg.restBeforePullManaPct = 55; // needs mana for heal-out-of-form
                return cfg;
            }
        };

    } // anonymous namespace

    std::unique_ptr<IdleBotClassRoutine> IdleBotClassRoutine::Make(uint8_t classId)
    {
        switch (classId)
        {
            case 1: return std::make_unique<WarriorRoutine>();
            case 2: return std::make_unique<PaladinRoutine>();
            case 3: return std::make_unique<HunterRoutine>();
            case 4: return std::make_unique<RogueRoutine>();
            case 5: return std::make_unique<PriestRoutine>();
            case 6: return std::make_unique<ShamanRoutine>();
            case 7: return std::make_unique<MageRoutine>();
            case 8: return std::make_unique<WarlockRoutine>();
            case 9: return std::make_unique<DruidRoutine>();
            default: return std::make_unique<DefaultRoutine>();
        }
    }

} // namespace idlebot
