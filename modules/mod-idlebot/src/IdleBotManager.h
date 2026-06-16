#ifndef MOD_IDLEBOT_MANAGER_H
#define MOD_IDLEBOT_MANAGER_H

#include "IdleBotPlayerbotBridge.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace idlebot
{
    // Per-bot runtime record. Persisted in idlebot_bots / idlebot_goals tables.
    struct BotRecord
    {
        std::string name;
        BotGuid guid = 0;
        bool active = false;
        bool paused = false;

        // current goal (M3+). Empty until a goal is set.
        std::string guideId;
        uint32_t currentStepIndex = 0;

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

        bool IsEnabled() const { return _enabled; }

    private:
        IdleBotManager() = default;

        void Tick();                  // advance all active bots one step
        void TickBot(BotRecord& rec); // advance a single bot (M3+ uses executor)

        bool _enabled = false;
        uint32_t _tickMs = 1000;
        uint32_t _accumMs = 0;
        uint32_t _maxActiveBots = 5;

        std::unique_ptr<IdleBotPlayerbotBridge> _bridge;
        std::unordered_map<std::string, BotRecord> _bots;
    };
}

#define sIdleBotMgr idlebot::IdleBotManager::instance()

#endif // MOD_IDLEBOT_MANAGER_H
