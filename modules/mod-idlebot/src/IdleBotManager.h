#ifndef MOD_IDLEBOT_MANAGER_H
#define MOD_IDLEBOT_MANAGER_H

#include "IdleBotPlayerbotBridge.h"
#include "IdleBotGuide.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <cctype>

namespace idlebot
{
    // Player-like death recovery phases. The playerbots DeadStrategy performs the
    // mechanics (auto release → find corpse → revive); idlebot observes, counts,
    // and nudges/falls back when recovery stalls.
    enum class DeathPhase
    {
        Alive,        // normal operation
        Dying,        // dead, corpse not yet released (let playerbots auto-release)
        Ghost,        // released; corpse run in progress
        Recovered     // transient: just came back alive (emit RECOVERY, then Alive)
    };

    // Per-bot runtime record. Persisted in idlebot_bots.
    struct BotRecord
    {
        std::string name;
        BotGuid guid = 0;
        bool active = false;
        bool paused = false;

        // current goal (M3+). Empty until a goal is set.
        std::string guideId;
        uint32_t currentStepIndex = 0;
        std::string stepState = "idle";   // idle/running/blocked (persisted)

        // --- death handling state machine (M4) ---
        DeathPhase deathPhase = DeathPhase::Alive;
        uint32_t deathCountTotal = 0;
        uint32_t deathCountStep = 0;          // deaths since entering current step
        uint32_t corpseRunAttempts = 0;       // idlebot revive nudges this death
        uint32_t ghostTicks = 0;              // ticks spent as ghost (stall detection)
        uint32_t lastDeathMap = 0;
        float lastDeathX = 0.f, lastDeathY = 0.f, lastDeathZ = 0.f;

        // --- delta polling for IdleRPG event log (M6) ---
        bool deltasInitialized = false;
        uint32_t lastLevel = 0;
        uint32_t lastQuestCount = 0;
        uint32_t lastFreeSlots = 0;
        bool strategiesEnsured = false;       // +loot/positioning toggled once per session
        // "strict" = idlebot drives explicit guide steps; "organic" = hand quest
        // pickup/travel/combat to playerbots' autonomous "new rpg" + grind AI and
        // just supervise (death, visibility, anti-stuck).
        std::string decisionMode = "strict";
        bool organicStrategiesEnsured = false; // +new rpg/+grind toggled once per session
        uint32_t strayTicks = 0;               // organic: ticks idle/resting w/ no quests
        uint32_t hubSteerCooldown = 0;         // organic: ticks before we may steer again
        bool maintaining = false;              // organic: on a vendor/repair/trainer town trip
        uint32_t maintTicks = 0;               // ticks spent on the current trip (timeout)
        uint32_t lastTrainedLevel = 0;         // level at which we last learned class spells
        uint32_t lastSpeccedLevel = 0;         // level at which we last auto-spent talents
        bool grindOn = false;                 // playerbots grind strategy on (kill steps only)
        bool aoeOn = false;                   // playerbots +aoe combat strategy on
        uint32_t lootGraceTicks = 0;          // hold position after a kill so the bot can loot
        uint32_t stuckTicks = 0;              // ticks with no attackable target (→ roam)
        uint32_t combatStallTicks = 0;        // ticks engaged with flat hp + no progress (frozen-AI breaker)
        float lastCombatHpPct = -1.f;         // hp last combat tick; flat hp while engaged => stalled rotation
        bool skipQuestRequested = false;      // HandleDeath flagged this quest unwinnable -> skip (not dead-stop)
        uint32_t reactivePinTicks = 0;        // ticks the reactive block has deferred a non-kill step (anti-pin)
        uint32_t retreatTicks = 0;            // ticks left in a kill-step tactical retreat
        uint32_t stepElapsedMs = 0;           // ACTIVE time on current step (watchdog; frozen while offline)
        uint32_t lastObjectiveCurrent = 0;    // last seen kill-objective count (watchdog progress reset)
        bool starterKitDone = false;          // gear/spells/talents applied once this process (not per reconnect)
        uint32_t dbgThrottle = 0;             // rate-limits the kill-step debug log
        uint32_t loginRetryTicks = 0;         // throttle AddPlayerBot while login is pending
        uint32_t controlWaitTicks = 0;        // throttle online-but-not-controlled diagnostics
        bool controlWaitArmed = false;        // waited once for AI; release+re-add if still none

        // contested gameobject handling (InteractGameObject steps)
        uint32_t objectWaitMs = 0;
        uint32_t lastObjectRetryMs = 0;
        uint32_t lastObjectRoamMs = 0;
        uint32_t objectAttemptsCurrentStep = 0;
        uint64_t lastObjectGuid = 0;
        std::string lastObjectFailureReason;

        // Fallback kill-credit tracking when quest credit lags behind actual
        // corpse loot. Counts only corpses that match the current kill step.
        uint32_t observedKillLootsCurrentStep = 0;
        uint64_t lastObservedKillLootGuid = 0;
    };

    // IdleBotManager
    // -------------------------------------------------------------------------
    // Owns the bot registry and the tick loop. The tick loop must NEVER block
    // the worldserver thread: it does at most one small action per bot per tick.
    //
    // Wired into AzerothCore via a WorldScript's OnUpdate (see IdleBotModule.cpp).
    class IdleBotManager
    {
    public:
        static IdleBotManager* instance();

        void Initialize();            // load config, construct bridge, load persisted bots
        void Shutdown();

        // Called from WorldScript::OnUpdate every server diff. Accumulates diff
        // and only advances bots once TickMs has elapsed.
        void OnWorldUpdate(uint32_t diffMs);

        // --- command-facing API (used by IdleBotCommandScript) ---
        bool AddBot(const std::string& name, std::string& outErr);
        bool RemoveBot(const std::string& name, std::string& outErr);
        std::string ListBots() const;            // human-readable for chat
        std::string StatusOf(const std::string& name) const;
        bool GetLivePosition(const std::string& name, BotPosition& out, std::string& outErr) const;
        bool PauseBot(const std::string& name);
        bool ResumeBot(const std::string& name);
        // Force gear + talent spec at the bot's CURRENT level (bypasses the L<=5
        // starter-kit gate). For testing high-level guides on a manually-leveled bot.
        bool GearBot(const std::string& name, std::string& outErr);

        // Is this character name a registered idlebot? Used by the chat-log hook.
        bool IsRegistered(const std::string& name) const
        {
            return _bots.count(NormalizeName(name)) != 0;
        }

        bool IsEnabled() const { return _enabled; }

        // Assign a guide to a bot. Guide must be registered via RegisterGuide.
        bool SetGuide(const std::string& botName, const std::string& guideId, std::string& outErr);
        bool ClearGuide(const std::string& botName, std::string& outErr);

        // Guide progress control (M3 persistence). Named SetGuideStep (not
        // GuideStep) to avoid shadowing the idlebot::GuideStep type inside the class.
        std::string GuideCurrent(const std::string& botName) const;
        bool GuideReset(const std::string& botName, std::string& outErr);
        bool SetGuideStep(const std::string& botName, uint32_t index, std::string& outErr);

        // IdleRPG feed (M6).
        std::string SummaryOf(const std::string& botName) const;

    private:
        IdleBotManager() = default;

        void Tick();                  // advance up to MaxActiveBots active bots one step
        void TickBot(BotRecord& rec, bool allowRuntime); // advance a single bot (M3+ uses executor)
        void LoadBots();              // load persisted registry from idlebot_bots
        void RegisterGuide(Guide g);  // add a guide to the in-memory registry
        void RegisterBuiltinGuides(); // called from Initialize
        void LoadConfiguredGuides();  // load file-based guides from IdleBot.GuideDirectory

        // Returns true if death handling consumed this tick (bot dead/recovering).
        bool HandleDeath(BotRecord& rec);
        // Bag-full / durability guard before quest/grind steps. Returns true if a
        // maintenance action is being performed (consume the tick).
        bool MaintenanceGuard(BotRecord& rec);
        // One-time per-session strategy setup (ensure looting on).
        void EnsureStrategies(BotRecord& rec);
        // Enable playerbots' autonomous questing AI for an organic-mode bot, once
        // per session. Returns true while organic mode owns the tick.
        bool TickOrganic(BotRecord& rec);
        // Player-like maintenance: run to a real merchant/repair NPC and use it
        // when gear is worn or bags are full. Returns true while on the trip (it
        // owns the bot's movement that tick). Only used when vendor-free is off.
        bool HandleVendorTrip(BotRecord& rec, BotLiveStatus const& st, InventoryStatus const& inv);
        // Poll level/quest/inventory deltas and emit IdleRPG events.
        void PollDeltas(BotRecord& rec);
        bool QuestObjectiveProgress(BotRecord& rec, GuideStep const& step, uint32_t& outCurrent, uint32_t& outRequired) const;
        bool CompletionConditionMet(BotRecord& rec, GuideStep const& step, uint32_t* outCurrent = nullptr, uint32_t* outRequired = nullptr) const;
        bool StepAppliesToBot(BotRecord const& rec, GuideStep const& step) const;
        bool StepHasCoordinates(GuideStep const& step) const;
        bool MoveToStepPosition(BotRecord& rec, GuideStep const& step, float minRadius) const;
        bool RewindToQuestAcceptStep(BotRecord& rec, Guide const& guide, uint32_t questId, char const* reason);
        // Move around a kill objective without drifting away from configured target creatures.
        void RoamKillObjective(BotRecord& rec, GuideStep const& step);
        // InteractGameObject step handler with player-like respawn waiting.
        bool HandleInteractGameObjectStep(BotRecord& rec, Guide const& guide, GuideStep const& step);
        void ResetObjectStepState(BotRecord& rec);
        uint32_t GameObjectMaxWaitMs(GuideStep const& step) const;
        bool GameObjectStepSkippable(GuideStep const& step) const;
        // Emit a categorized IdleRPG event (per-bot log + idlebot_events table).
        void EmitEvent(const BotRecord& rec, const char* category, const std::string& message);
        // Persist guide progress + death counters to idlebot_bots.
        void PersistProgress(const BotRecord& rec);
        // Advance to the next step (resets per-step death counter + persists).
        void AdvanceStep(BotRecord& rec);
        // Skip every consecutive step belonging to questId (used when a quest
        // can't be accepted, e.g. an unmet prerequisite in a generated guide).
        void SkipQuestSteps(BotRecord& rec, Guide const& guide, uint32_t questId);

        // WoW character name format: first char uppercase, rest lowercase, pure alpha.
        // Applied to every name that enters the registry so case never matters at call sites.
        static std::string NormalizeName(std::string name)
        {
            if (name.empty())
                return name;
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            for (std::size_t i = 1; i < name.size(); ++i)
                name[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
            return name;
        }

        bool _enabled = false;
        uint32_t _tickMs = 1000;
        uint32_t _accumMs = 0;
        uint32_t _maxActiveBots = 5;
        uint32_t _stepSkipSeconds = 2700;   // active-time floor before the step watchdog skips a stuck quest
        std::string _decisionMode = "strict";   // default mode persisted for new bots

        // death handling (Priority 2)
        bool _deathEnabled = true;
        bool _allowDirectResurrect = true;
        bool _allowGraveyardResurrect = true;
        uint32_t _maxCorpseRunAttempts = 3;
        uint32_t _maxDeathsPerStep = 3;
        bool _pauseAfterDeathLoop = true;
        uint32_t _ghostStallTicks = 8;          // ticks as ghost before idlebot nudges

        // inventory / town maintenance (Priority 5)
        bool _townMaintenanceEnabled = true;
        bool _vendorFreeMaintenance = false;    // true = magic repair/restock; false = run to a vendor
        uint32_t _minFreeSlotsBeforeQuest = 2;
        uint32_t _minFreeSlotsBeforeGrind = 4;
        uint32_t _repairBelowDurabilityPct = 40;

        // contested gameobject handling
        bool _gameObjectWaitForRespawn = true;
        uint32_t _gameObjectRetryEveryMs = 5000;
        uint32_t _gameObjectRoamEveryMs = 20000;
        uint32_t _gameObjectRequiredMaxWaitMs = 0;
        uint32_t _gameObjectOptionalMaxWaitMs = 15 * 60 * 1000;
        float _gameObjectDefaultSearchRadius = 60.f;
        float _gameObjectRoamRadius = 35.f;

        // telemetry (Priority 6)
        bool _eventsToDb = true;
        bool _debugEnabled = false;             // verbose kill-step diagnostics to the world log

        // adaptive combat (smart engagement modes)
        uint32_t _maxPull = 3;                  // attackers before we stop adding targets
        uint32_t _lowHpPct = 35;                // recover below this health %
        uint32_t _lowManaPct = 20;              // recover (drink) below this mana % when safe
        uint32_t _aoeThreshold = 3;             // cluster size to switch on +aoe
        bool _rangedKite = true;                // ranged classes back off when attacked
        bool _autoGear = false;                 // 0 = player-like (loot/vendor only)

        std::unique_ptr<IdleBotPlayerbotBridge> _bridge;
        std::unordered_map<std::string, BotRecord> _bots;
        std::unordered_map<std::string, Guide> _guides;
    };
}

#define sIdleBotMgr idlebot::IdleBotManager::instance()

#endif // MOD_IDLEBOT_MANAGER_H
