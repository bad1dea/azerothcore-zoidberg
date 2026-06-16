#include "IdleBotManager.h"
#include "IdleBotLog.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include "StringFormat.h"

// Config + logging headers verified in this checkout:
//   src/common/Configuration/Config.h   -> sConfigMgr->GetOption<T>(name, default)
//   src/common/Logging/Log.h            -> LOG_INFO("category", "msg {}", arg)
//
// Guide step executor (M3):
//   Bridge calls verified in IdleBotPlayerbotBridge.cpp before implementation.

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

        // Register in-memory builtin guides (YAML loading is M5).
        RegisterBuiltinGuides();

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
        // Always ensure the bot is online first (idempotent).
        if (_bridge)
        {
            if (_bridge->EnsureBotOnline(rec.name) && !rec.guid)
                rec.guid = _bridge->GetBotGuid(rec.name);
        }

        if (!_bridge || !rec.guid)
            return;

        // M3: guide step executor. One step at a time; never advance more than
        // one step per tick so the world thread isn't held up.
        if (rec.guideId.empty())
            return;

        auto git = _guides.find(rec.guideId);
        if (git == _guides.end())
        {
            LOG_WARN("module.idlebot", "[IdleBot] bot '{}': guide '{}' not found — clearing.", rec.name, rec.guideId);
            rec.guideId.clear();
            rec.currentStepIndex = 0;
            return;
        }

        const Guide& guide = git->second;
        if (rec.currentStepIndex >= guide.steps.size())
        {
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': guide '{}' complete!", rec.name, rec.guideId);
            sIdleBotLog->Write(rec.name, "GUIDE", Acore::StringFormat("guide '{}' complete", rec.guideId));
            rec.guideId.clear();
            rec.currentStepIndex = 0;
            return;
        }

        const GuideStep& step = guide.steps[rec.currentStepIndex];
        bool stepDone = false;

        switch (step.type)
        {
        case StepType::MoveTo:
        {
            // Check arrival first; if not there, issue the move command.
            BotPosition pos = _bridge->GetPosition(rec.guid);
            if (pos.valid && pos.mapId == step.coords.mapId)
            {
                float dx = pos.x - step.coords.x;
                float dy = pos.y - step.coords.y;
                float dz = pos.z - step.coords.z;
                float dist2 = dx * dx + dy * dy + dz * dz;
                float r = step.coords.radius;
                if (dist2 <= r * r)
                {
                    stepDone = true;
                    break;
                }
            }
            _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, step.coords.radius);
            break;
        }

        case StepType::AcceptQuest:
        {
            if (!step.questId.has_value())
                { stepDone = true; break; }  // malformed step — skip

            uint32_t qid = *step.questId;
            QuestState qs = _bridge->GetQuestStatus(rec.guid, qid);
            if (qs != QuestState::NotStarted && qs != QuestState::Unknown)
            {
                stepDone = true;  // already accepted (or rewarded)
                break;
            }
            uint32_t entry = step.npcId.value_or(0);
            _bridge->AcceptQuest(rec.guid, qid, entry);
            break;
        }

        case StepType::TurnInQuest:
        {
            if (!step.questId.has_value())
                { stepDone = true; break; }

            uint32_t qid = *step.questId;
            QuestState qs = _bridge->GetQuestStatus(rec.guid, qid);
            if (qs == QuestState::Rewarded)
            {
                stepDone = true;
                break;
            }
            if (qs != QuestState::Complete)
                break;  // not yet ready to turn in

            uint32_t entry = step.npcId.value_or(0);
            _bridge->TurnInQuest(rec.guid, qid, entry);
            break;
        }

        case StepType::WaitForQuestComplete:
        {
            if (!step.questId.has_value())
                { stepDone = true; break; }

            QuestState qs = _bridge->GetQuestStatus(rec.guid, *step.questId);
            stepDone = (qs == QuestState::Complete || qs == QuestState::Rewarded);
            break;
        }

        default:
            // Unknown / not-yet-implemented step types: log and skip.
            LOG_WARN("module.idlebot", "[IdleBot] bot '{}': step type {} not implemented — skipping.",
                rec.name, static_cast<int>(step.type));
            stepDone = true;
            break;
        }

        if (stepDone)
        {
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': step {}/{} '{}' done.",
                rec.name, rec.currentStepIndex + 1, guide.steps.size(), step.name);
            sIdleBotLog->Write(rec.name, "STEP", Acore::StringFormat("step {}/{} '{}' done",
                rec.currentStepIndex + 1, guide.steps.size(), step.name));
            ++rec.currentStepIndex;
        }
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

    bool IdleBotManager::AddBot(const std::string& rawName, std::string& outErr)
    {
        std::string name = NormalizeName(rawName);

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

    bool IdleBotManager::RemoveBot(const std::string& rawName, std::string& outErr)
    {
        std::string const name = NormalizeName(rawName);
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
        for (auto const& [name, rec] : _bots)
        {
            out += "\n  " + name
                 + (rec.active ? " [active]" : " [inactive]")
                 + (rec.paused ? " [paused]" : "");
        }
        return out;
    }

    std::string IdleBotManager::StatusOf(const std::string& rawName) const
    {
        std::string const name = NormalizeName(rawName);
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

    bool IdleBotManager::PauseBot(const std::string& rawName)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = true;
        sIdleBotLog->Write(name, "EVENT", "paused");
        return true;
    }

    bool IdleBotManager::ResumeBot(const std::string& rawName)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = false;
        sIdleBotLog->Write(name, "EVENT", "resumed");
        return true;
    }

    bool IdleBotManager::SetGuide(const std::string& rawName, const std::string& guideId, std::string& outErr)
    {
        std::string const name = NormalizeName(rawName);
        auto bit = _bots.find(name);
        if (bit == _bots.end())
        {
            outErr = "no such bot";
            return false;
        }
        if (!_guides.count(guideId))
        {
            outErr = "unknown guide '" + guideId + "'";
            return false;
        }
        bit->second.guideId = guideId;
        bit->second.currentStepIndex = 0;
        sIdleBotLog->Write(name, "GUIDE", "assigned guide '" + guideId + "'");
        LOG_INFO("module.idlebot", "[IdleBot] bot '{}': guide set to '{}'.", name, guideId);
        return true;
    }

    bool IdleBotManager::ClearGuide(const std::string& rawName, std::string& outErr)
    {
        std::string const name = NormalizeName(rawName);
        auto bit = _bots.find(name);
        if (bit == _bots.end())
        {
            outErr = "no such bot";
            return false;
        }
        bit->second.guideId.clear();
        bit->second.currentStepIndex = 0;
        sIdleBotLog->Write(name, "GUIDE", "guide cleared");
        return true;
    }

    void IdleBotManager::RegisterGuide(Guide g)
    {
        std::string id = g.id;
        _guides.emplace(std::move(id), std::move(g));
    }

    void IdleBotManager::RegisterBuiltinGuides()
    {
        // test guide: moves the bot 10 yards east — validates the executor without
        // requiring real quest data. Replace coordinates as needed for your bot's
        // current position.
        {
            Guide g;
            g.id = "test";
            g.name = "Movement smoke test";

            GuideStep step;
            step.id = "test_move";
            step.name = "move 10 yards east";
            step.type = StepType::MoveTo;
            step.coords.mapId = 0;     // Eastern Kingdoms
            step.coords.x = 1686.7f;  // bot's last known x + 10
            step.coords.y = 1678.3f;
            step.coords.z = 121.7f;
            step.coords.radius = 3.f;
            g.steps.push_back(step);

            RegisterGuide(std::move(g));
        }

        LOG_INFO("module.idlebot", "[IdleBot] registered {} builtin guide(s).", _guides.size());
    }
}
