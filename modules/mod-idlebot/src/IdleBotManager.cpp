#include "IdleBotManager.h"

// TODO(verify): AzerothCore includes for config + logging. Confirm exact headers
// and macro names in your checkout:
//   #include "Config.h"      -> sConfigMgr->GetOption<...>(...)
//   #include "Log.h"         -> LOG_INFO / LOG_ERROR with a logger channel
// Names below assume current AC; grep to confirm before relying on them.
//
// #include "Config.h"
// #include "Log.h"

namespace idlebot
{
    IdleBotManager* IdleBotManager::instance()
    {
        static IdleBotManager mgr;
        return &mgr;
    }

    void IdleBotManager::Initialize()
    {
        // TODO(verify): read from sConfigMgr. Placeholder defaults until wired.
        // _enabled       = sConfigMgr->GetOption<bool>("IdleBot.Enabled", false);
        // _tickMs        = sConfigMgr->GetOption<uint32_t>("IdleBot.TickMs", 1000);
        // _maxActiveBots = sConfigMgr->GetOption<uint32_t>("IdleBot.MaxActiveBots", 5);
        // std::string ctrl = sConfigMgr->GetOption<std::string>("IdleBot.ControlMode", "chat");

        if (!_enabled)
        {
            // LOG_INFO("module.idlebot", "[IdleBot] disabled by config.");
            return;
        }

        // _bridge.reset(CreateBridge(ctrl));

        // TODO(M2+): load persisted bots from idlebot_bots table.
        // LOG_INFO("module.idlebot", "[IdleBot] initialized. tick=%ums max=%u", _tickMs, _maxActiveBots);
    }

    void IdleBotManager::Shutdown()
    {
        // TODO(M5): flush state snapshots to DB so goals resume after restart.
        _bots.clear();
        _bridge.reset();
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
        return true;
    }

    bool IdleBotManager::ResumeBot(const std::string& name)
    {
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = false;
        return true;
    }
}
