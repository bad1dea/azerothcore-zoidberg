#include "IdleBotManager.h"
#include "IdleBotLog.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "StringFormat.h"

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
        _decisionMode  = sConfigMgr->GetOption<std::string>("IdleBot.DecisionMode", "strict");
        _accumMs       = 0;

        // Per-bot logging works even when the module itself is disabled (commands
        // still register bots), so initialize it before the early-return below.
        sIdleBotLog->Initialize();

        // Build the playerbot bridge (the seam to mod-playerbots). Constructed
        // even when disabled so `.idlebot status` can read live state of bots
        // that are online by other means. Skip only in dry-run.
        if (sConfigMgr->GetOption<bool>("IdleBot.UsePlayerbots", true))
        {
            std::string ctrl = sConfigMgr->GetOption<std::string>("IdleBot.ControlMode", "chat");
            _bridge.reset(CreateBridge(ctrl));
        }

        // Restore the registry so bots survive a worldserver restart.
        LoadBots();

        if (!_enabled)
        {
            LOG_INFO("module.idlebot", "[IdleBot] disabled by config (IdleBot.Enabled = 0). Commands still respond.");
            return;
        }

        LOG_INFO("module.idlebot", "[IdleBot] initialized. tick={}ms maxActiveBots={} bridge={}",
            _tickMs, _maxActiveBots, _bridge ? "on" : "off");
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
        // M2: keep the bot under control. One small, idempotent action per tick —
        // EnsureBotOnline early-returns if the bot is already in world, and the
        // underlying master-less add guards against duplicate logins.
        if (_bridge)
        {
            if (_bridge->EnsureBotOnline(rec.name) && !rec.guid)
                rec.guid = _bridge->GetBotGuid(rec.name);
        }
        // M3+: read current guide step -> (M7+) decision engine -> executor does
        // ONE small action -> snapshot state.
    }

    void IdleBotManager::LoadBots()
    {
        // Best-effort: if the idlebot_bots table is absent the query returns null
        // and we simply start with an empty registry.
        QueryResult result = CharacterDatabase.Query("SELECT bot_name, active FROM idlebot_bots");
        if (!result)
            return;

        uint32_t loaded = 0;
        do
        {
            Field* fields = result->Fetch();
            std::string name = fields[0].Get<std::string>();
            if (_bots.count(name))
                continue;

            BotRecord rec;
            rec.name = name;
            rec.active = fields[1].Get<bool>();
            _bots.emplace(name, std::move(rec));
            ++loaded;
        } while (result->NextRow());

        LOG_INFO("module.idlebot", "[IdleBot] loaded {} persisted bot(s).", loaded);
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

        // Persist (best-effort, async). Character names are constrained to a safe
        // charset by the client, so direct interpolation is acceptable here.
        CharacterDatabase.Execute(
            "INSERT INTO idlebot_bots (bot_name, active, decision_mode) VALUES ('{}', 1, '{}') "
            "ON DUPLICATE KEY UPDATE active = 1",
            name, _decisionMode);
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
        // Hand control back (log the bot out) before forgetting it.
        if (_bridge)
            _bridge->ReleaseBot(name);

        _bots.erase(it);
        sIdleBotLog->Write(name, "EVENT", "removed from idlebot");
        CharacterDatabase.Execute("DELETE FROM idlebot_bots WHERE bot_name = '{}'", name);
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
        out += rec.paused ? " [paused]" : (rec.active ? " [active]" : " [inactive]");

        if (_bridge)
        {
            BotGuid guid = _bridge->GetBotGuid(name);
            if (!guid)
            {
                out += "\n  character: not found on this realm";
            }
            else
            {
                BotLiveStatus st;
                _bridge->GetLiveStatus(guid, st);
                if (!st.online)
                {
                    out += "\n  offline";
                }
                else
                {
                    out += st.controlled ? "\n  online [bot-controlled]" : "\n  online [NOT bot-controlled]";
                    out += Acore::StringFormat("\n  level {}  hp {}/{}  mana {}/{}",
                        st.level, st.health, st.maxHealth, st.mana, st.maxMana);
                    out += Acore::StringFormat("\n  quests: {}", st.questCount);
                    if (st.pos.valid)
                        out += Acore::StringFormat("\n  pos: map {} ({:.1f}, {:.1f}, {:.1f})",
                            st.pos.mapId, st.pos.x, st.pos.y, st.pos.z);
                }
            }
        }

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
