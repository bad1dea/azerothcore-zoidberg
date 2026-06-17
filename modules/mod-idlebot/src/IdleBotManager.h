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
        bool grindOn = false;                 // playerbots grind strategy on (kill steps only)
        bool aoeOn = false;                   // playerbots +aoe combat strategy on
        uint32_t lootGraceTicks = 0;          // hold position after a kill so the bot can loot
        uint32_t stuckTicks = 0;              // ticks with no attackable target (→ roam)
        uint32_t dbgThrottle = 0;             // rate-limits the kill-step debug log

        // --- contested gameobject handling ---
        uint32_t objectWaitMs = 0;
        uint32_t lastObjectRetryMs = 0;
        uint32_t lastObjectRoamMs = 0;
        uint32_t objectAttemptsCurrentStep = 0;
        uint64_t lastObjectGuid = 0;
        std::string lastObjectFailureReason;
        uint32_t lastObjectProgressCount = 0;

        // bookkeeping for non-blocking tick scheduling
        uint32_t msSinceLastAction = 0;
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
        bool PauseBot(const std::string& name);
        bool ResumeBot(const std::string& name);

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

        void Tick();                  // advance all active bots one step
        void TickBot(BotRecord& rec); // advance a single bot (M3+ uses executor)
        void LoadBots();              // load persisted registry from idlebot_bots
        void RegisterGuide(Guide g);  // add a guide to the in-memory registry
        void RegisterBuiltinGuides(); // called from Initialize

        // Returns true if death handling consumed this tick (bot dead/recovering).
        bool HandleDeath(BotRecord& rec);
        // Bag-full / durability guard before quest/grind steps. Returns true if a
        // maintenance action is being performed (consume the tick).
        bool MaintenanceGuard(BotRecord& rec);
        // One-time per-session strategy setup (ensure looting on).
        void EnsureStrategies(BotRecord& rec);
        // Poll level/quest/inventory deltas and emit IdleRPG events.
        void PollDeltas(BotRecord& rec);
        // Emit a categorized IdleRPG event (per-bot log + idlebot_events table).
        void EmitEvent(const BotRecord& rec, const char* category, const std::string& message);
        // Persist guide progress + death counters to idlebot_bots.
        void PersistProgress(const BotRecord& rec);
        // Advance to the next step (resets per-step death counter + persists).
        void AdvanceStep(BotRecord& rec);
        void ResetStepRuntimeState(BotRecord& rec);

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
        uint32_t _minFreeSlotsBeforeQuest = 2;
        uint32_t _minFreeSlotsBeforeGrind = 4;
        uint32_t _repairBelowDurabilityPct = 40;

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

        // contested gameobjects
        bool _objectWaitForRespawn = true;
        uint32_t _objectRetryEveryMs = 5000;
        uint32_t _objectRoamEveryMs = 20000;
        uint32_t _objectRequiredMaxWaitMs = 0;  // 0 = indefinitely
        uint32_t _objectOptionalMaxWaitMs = 15 * 60 * 1000;
        float _objectDefaultSearchRadius = 60.f;
        float _objectRoamRadius = 35.f;

        std::unique_ptr<IdleBotPlayerbotBridge> _bridge;
        std::unordered_map<std::string, BotRecord> _bots;
        std::unordered_map<std::string, Guide> _guides;
    };
}

#define sIdleBotMgr idlebot::IdleBotManager::instance()

#endif // MOD_IDLEBOT_MANAGER_H
