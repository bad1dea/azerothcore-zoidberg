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

        // Is this character name a registered idlebot? Used by the chat-log hook.
        bool IsRegistered(const std::string& name) const
        {
            return _bots.count(NormalizeName(name)) != 0;
        }

        bool IsEnabled() const { return _enabled; }

        // Assign a guide to a bot. Guide must be registered via RegisterGuide.
        bool SetGuide(const std::string& botName, const std::string& guideId, std::string& outErr);
        bool ClearGuide(const std::string& botName, std::string& outErr);

    private:
        IdleBotManager() = default;

        void Tick();                  // advance all active bots one step
        void TickBot(BotRecord& rec); // advance a single bot (M3+ uses executor)
        void LoadBots();              // load persisted registry from idlebot_bots
        void RegisterGuide(Guide g);  // add a guide to the in-memory registry
        void RegisterBuiltinGuides(); // called from Initialize

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

        std::unique_ptr<IdleBotPlayerbotBridge> _bridge;
        std::unordered_map<std::string, BotRecord> _bots;
        std::unordered_map<std::string, Guide> _guides;
    };
}

#define sIdleBotMgr idlebot::IdleBotManager::instance()

#endif // MOD_IDLEBOT_MANAGER_H
