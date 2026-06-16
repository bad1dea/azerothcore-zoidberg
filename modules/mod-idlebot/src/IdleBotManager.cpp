#include "IdleBotManager.h"
#include "IdleBotLog.h"
#include "Configuration/Config.h"
#include "Log.h"

// Config + logging headers verified in this checkout:
//   src/common/Configuration/Config.h   -> sConfigMgr->GetOption<T>(name, default)
//   src/common/Logging/Log.h            -> LOG_INFO("category", "msg {}", arg)

namespace idlebot
{
    IdleBotManager* IdleBotManager::instance()
    {
        static IdleBotManager mgr;
        return &mgr;
    }

    void IdleBotManager::Initialize()
    {
        _enabled       = sConfigMgr->GetOption<bool>("IdleBot.Enabled", false);
        _tickMs        = sConfigMgr->GetOption<uint32_t>("IdleBot.TickMs", 1000);
        _maxActiveBots = sConfigMgr->GetOption<uint32_t>("IdleBot.MaxActiveBots", 5);
        _accumMs       = 0;

        // Per-bot logging works even when the module itself is disabled (commands
        // still register bots), so initialize it before the early-return below.
        sIdleBotLog->Initialize();

        if (!_enabled)
        {
            LOG_INFO("module.idlebot", "[IdleBot] disabled by config (IdleBot.Enabled = 0). Commands still respond.");
            return;
        }

        // NOTE: the playerbot bridge (CreateBridge) is not constructed yet — it
        // lands in M2/M3 once the mod-playerbots command surface is wired. Until
        // then the manager runs registry-only; the tick is a no-op per bot.
        LOG_INFO("module.idlebot", "[IdleBot] initialized. tick={}ms maxActiveBots={}", _tickMs, _maxActiveBots);
    }

    void IdleBotManager::Shutdown()
    {
        // TODO(M5): flush state snapshots to DB so goals resume after restart.
        _bots.clear();
        _bridge.reset();
        sIdleBotLog->Shutdown();
    }

    void IdleBotManager::OnWorldUpdate(uint32_t diffMs)
    {
        if (!_enabled)
            return;

        _accumMs += diffMs;
        if (_accumMs < _tickMs)
            return;

        _accumMs = 0;
        Tick();
    }

    void IdleBotManager::Tick()
    {
        // One small action per active bot. Never loop until "done" — that would
        // block the world thread. Each TickBot does at most one step.
        for (auto& [name, rec] : _bots)
        {
            if (!rec.active || rec.paused)
                continue;
            TickBot(rec);
        }
    }

    void IdleBotManager::TickBot(BotRecord& rec)
    {
        // M2: nothing to do beyond keeping the record alive.
        // M3+: hand off to IdleBotStepExecutor / IdleBotDecisionEngine here.
        //   1. ensure bot online via _bridge->EnsureBotOnline(rec.name)
        //   2. read current guide step
        //   3. (M7+) decision engine evaluates context
        //   4. executor performs ONE small action
        //   5. snapshot state
        (void)rec;
    }

    bool IdleBotManager::AddBot(const std::string& name, std::string& outErr)
    {
        if (_bots.size() >= _maxActiveBots)
        {
            outErr = "max active bots reached";
            return false;
        }
        if (_bots.count(name))
        {
            outErr = "bot already registered";
            return false;
        }

        BotRecord rec;
        rec.name = name;
        rec.active = true;
        _bots.emplace(name, std::move(rec));

        sIdleBotLog->Write(name, "EVENT", "registered with idlebot");
        // TODO(M2): persist to idlebot_bots; resolve guid via bridge if online.
        return true;
    }

    bool IdleBotManager::RemoveBot(const std::string& name, std::string& outErr)
    {
        auto it = _bots.find(name);
        if (it == _bots.end())
        {
            outErr = "no such bot";
            return false;
        }
        _bots.erase(it);
        sIdleBotLog->Write(name, "EVENT", "removed from idlebot");
        // TODO(M2): delete/deactivate row in idlebot_bots.
        return true;
    }

    std::string IdleBotManager::ListBots() const
    {
        if (_bots.empty())
            return "No bots registered.";

        std::string out = "Registered bots:";
        for (const auto& [name, rec] : _bots)
        {
            out += "\n  " + name
                 + (rec.active ? " [active]" : " [inactive]")
                 + (rec.paused ? " [paused]" : "");
        }
        return out;
    }

    std::string IdleBotManager::StatusOf(const std::string& name) const
    {
        auto it = _bots.find(name);
        if (it == _bots.end())
            return "No such bot: " + name;

        const BotRecord& rec = it->second;
        std::string out = "Bot " + name + ":";
        out += rec.paused ? " paused" : (rec.active ? " active" : " inactive");
        // TODO(M2): pull live level/HP/mana/position/quests via _bridge.
        out += "\n  guide: " + (rec.guideId.empty() ? "(none)" : rec.guideId);
        return out;
    }

    bool IdleBotManager::PauseBot(const std::string& name)
    {
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = true;
        sIdleBotLog->Write(name, "EVENT", "paused");
        return true;
    }

    bool IdleBotManager::ResumeBot(const std::string& name)
    {
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = false;
        sIdleBotLog->Write(name, "EVENT", "resumed");
        return true;
    }
}
