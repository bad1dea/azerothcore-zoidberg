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

        case StepType::KillMobs:
        {
            // mod-playerbots random bot AI handles combat autonomously. The executor
            // just waits for the associated quest to reach Complete or Rewarded.
            // If no questId, fall through and skip the step (nothing to check).
            if (!step.questId.has_value())
            {
                stepDone = true;
                break;
            }
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
        // requiring real quest data.
        {
            Guide g;
            g.id = "test";
            g.name = "Movement smoke test";

            GuideStep step;
            step.id = "test_move";
            step.name = "move 10 yards east";
            step.type = StepType::MoveTo;
            step.coords.mapId = 0;
            step.coords.x = 1686.7f;
            step.coords.y = 1678.3f;
            step.coords.z = 121.7f;
            step.coords.radius = 3.f;
            g.steps.push_back(step);

            RegisterGuide(std::move(g));
        }

        // horde-1-12-tirisfal-glades
        // All NPC entries and positions verified from acore_world DB on zoidberg.
        // NPC entries: Sarvis=1569, Elreth=1661, Saltain=1740, Arren=1570
        //   Zygand=1515, Johaan=1518, Dillinger=1496, Burgess=1652, Sevren=1499
        // Quest IDs verified from creature_queststarter/creature_questender tables.
        // NOTE: Quest 3902 (Scavenging Deathknell) uses game object interaction —
        //   the bot will accept it but the KillMobs step will never complete until
        //   InteractGameobject is implemented (M5+). Included for quest chain integrity.
        {
            Guide g;
            g.id = "horde-1-12-tirisfal-glades";
            g.name = "Horde 1-12 Tirisfal Glades (Deathknell → Brill)";
            g.faction = "horde";
            g.race = "undead";
            g.levelMin = 1;
            g.levelMax = 12;

            auto mv = [](std::string id, std::string name, uint32_t map, float x, float y, float z, float r = 5.f) {
                GuideStep s;
                s.id = std::move(id);
                s.name = std::move(name);
                s.type = StepType::MoveTo;
                s.coords = { map, x, y, z, r, false };
                return s;
            };
            auto aq = [](std::string id, std::string name, uint32_t quest, uint32_t npc, uint32_t map, float x, float y, float z) {
                GuideStep s;
                s.id = std::move(id);
                s.name = std::move(name);
                s.type = StepType::AcceptQuest;
                s.questId = quest;
                s.npcId = npc;
                s.coords = { map, x, y, z, 5.5f, false };
                return s;
            };
            auto ki = [](std::string id, std::string name, uint32_t quest, uint32_t map, float x, float y, float z, float r = 60.f) {
                GuideStep s;
                s.id = std::move(id);
                s.name = std::move(name);
                s.type = StepType::KillMobs;
                s.questId = quest;
                s.coords = { map, x, y, z, r, false };
                return s;
            };
            auto tq = [](std::string id, std::string name, uint32_t quest, uint32_t npc, uint32_t map, float x, float y, float z) {
                GuideStep s;
                s.id = std::move(id);
                s.name = std::move(name);
                s.type = StepType::TurnInQuest;
                s.questId = quest;
                s.npcId = npc;
                s.coords = { map, x, y, z, 5.5f, false };
                return s;
            };

            // ---- Deathknell ----

            // Q364: The Mindless Ones (L2) — Sarvis gives, 5 Zombies + 5 Ghouls
            g.steps.push_back(mv("q364_go_sarvis", "go to Executor Sarvis for The Mindless Ones",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(aq("q364_accept", "accept The Mindless Ones (364)",
                364, 1569, 0, 1843.32f, 1639.9f, 97.8f));
            g.steps.push_back(mv("q364_go_zombies", "go to zombie/ghoul area",
                0, 1924.f, 1558.f, 84.f, 80.f));
            g.steps.push_back(ki("q364_kill", "kill Mindless Zombies and Wretched Ghouls (q364)",
                364, 0, 1924.f, 1558.f, 84.f, 80.f));
            g.steps.push_back(mv("q364_return_sarvis", "return to Executor Sarvis",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(tq("q364_turnin", "turn in The Mindless Ones (364)",
                364, 1569, 0, 1843.32f, 1639.9f, 97.8f));

            // Q376: Rattling the Rattlecages (L2) → Q3901 prereq, Elreth gives, 6 bat wings + 6 paws
            g.steps.push_back(mv("q376_go_elreth", "go to Novice Elreth for Rattling the Rattlecages",
                0, 1847.73f, 1638.65f, 97.0f, 6.f));
            g.steps.push_back(aq("q376_accept", "accept Rattling the Rattlecages (376)",
                376, 1661, 0, 1847.73f, 1638.65f, 97.0f));
            g.steps.push_back(mv("q376_go_bats", "go to duskbat area",
                0, 1883.f, 1624.f, 102.f, 60.f));
            g.steps.push_back(ki("q376_kill", "kill Duskbats and Young Scavengers (q376)",
                376, 0, 1918.f, 1590.f, 93.f, 80.f));
            g.steps.push_back(mv("q376_return_elreth", "return to Novice Elreth",
                0, 1847.73f, 1638.65f, 97.0f, 6.f));
            g.steps.push_back(tq("q376_turnin", "turn in Rattling the Rattlecages (376)",
                376, 1661, 0, 1847.73f, 1638.65f, 97.0f));

            // Q3901: Graverobbers (L3) — Sarvis gives, 8 Rattlecage Skeletons
            g.steps.push_back(mv("q3901_go_sarvis", "go to Executor Sarvis for Graverobbers",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(aq("q3901_accept", "accept Graverobbers (3901)",
                3901, 1569, 0, 1843.32f, 1639.9f, 97.8f));
            g.steps.push_back(mv("q3901_go_skeletons", "go to Rattlecage Skeleton area",
                0, 1979.f, 1542.f, 81.f, 60.f));
            g.steps.push_back(ki("q3901_kill", "kill Rattlecage Skeletons (q3901)",
                3901, 0, 1979.f, 1542.f, 81.f, 80.f));
            g.steps.push_back(mv("q3901_return_sarvis", "return to Executor Sarvis",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(tq("q3901_turnin", "turn in Graverobbers (3901)",
                3901, 1569, 0, 1843.32f, 1639.9f, 97.8f));

            // Q3902: Scavenging Deathknell (L3) — Saltain gives, 6 Scavenged Goods (game objects).
            // Accept only; no wait/turnin. Q380 requires only Q376, not Q3902, so we don't
            // need to complete this to continue the chain. The quest sits in the log until
            // InteractGameobject steps are added in M5.
            g.steps.push_back(mv("q3902_go_saltain", "go to Deathguard Saltain for Scavenging Deathknell",
                0, 1861.17f, 1605.02f, 95.0f, 6.f));
            g.steps.push_back(aq("q3902_accept", "accept Scavenging Deathknell (3902) — stays incomplete until M5",
                3902, 1740, 0, 1861.17f, 1605.02f, 95.0f));

            // Q380: Night Web's Hollow (L4) — Arren gives, 8 young spiders + 5 night spiders
            g.steps.push_back(mv("q380_go_arren", "go to Executor Arren for Night Web's Hollow",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q380_accept", "accept Night Web's Hollow (380)",
                380, 1570, 0, 1848.82f, 1580.47f, 94.7f));
            g.steps.push_back(mv("q380_go_spiders", "go to Night Web spider area",
                0, 2060.f, 1800.f, 90.f, 80.f));
            g.steps.push_back(ki("q380_kill", "kill Young Night Web Spiders and Night Web Spiders (q380)",
                380, 0, 2060.f, 1800.f, 90.f, 100.f));
            g.steps.push_back(mv("q380_return_arren", "return to Executor Arren",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(tq("q380_turnin", "turn in Night Web's Hollow (380)",
                380, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // Q381: The Scarlet Crusade (L4) — Arren gives, 12 Scarlet Armbands from Scarlet Converts/Initiates
            g.steps.push_back(mv("q381_go_arren", "go to Executor Arren for The Scarlet Crusade",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q381_accept", "accept The Scarlet Crusade (381)",
                381, 1570, 0, 1848.82f, 1580.47f, 94.7f));
            g.steps.push_back(mv("q381_go_scarlets", "go to Scarlet Convert/Initiate area",
                0, 1808.f, 1339.f, 90.f, 80.f));
            g.steps.push_back(ki("q381_kill", "kill Scarlet Converts and Initiates for armbands (q381)",
                381, 0, 1808.f, 1339.f, 90.f, 80.f));
            g.steps.push_back(mv("q381_return_arren", "return to Executor Arren",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(tq("q381_turnin", "turn in The Scarlet Crusade (381)",
                381, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // Q382: Vital Intelligence (L5) — Arren gives, kill Meven Korgal (elite 1667) for docs
            g.steps.push_back(mv("q382_go_arren", "go to Executor Arren for Vital Intelligence",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q382_accept", "accept Vital Intelligence (382)",
                382, 1570, 0, 1848.82f, 1580.47f, 94.7f));
            g.steps.push_back(mv("q382_go_korgal", "go to Meven Korgal",
                0, 1772.f, 1381.f, 91.f, 15.f));
            g.steps.push_back(ki("q382_kill", "kill Meven Korgal for Scarlet Crusade Documents (q382)",
                382, 0, 1772.f, 1381.f, 91.f, 20.f));
            g.steps.push_back(mv("q382_return_arren", "return to Executor Arren",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(tq("q382_turnin", "turn in Vital Intelligence (382)",
                382, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // Q383: Deliver Documents (L5) — Arren gives, run to Zygand in Brill
            g.steps.push_back(mv("q383_go_arren", "go to Executor Arren for Deliver Documents",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q383_accept", "accept Deliver Documents (383)",
                383, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // ---- Travel to Brill ----
            g.steps.push_back(mv("brill_travel", "travel road to Brill",
                0, 2278.08f, 295.587f, 35.3f, 15.f));

            // Q383 turn-in at Zygand
            g.steps.push_back(tq("q383_turnin", "turn in Deliver Documents (383) to Zygand",
                383, 1515, 0, 2278.08f, 295.587f, 35.3f));

            // ---- Brill quests ----

            // Q367: Johaan (L6) — 5 Darkhound Blood from Rot Hide Darkhounds
            g.steps.push_back(mv("q367_go_johaan", "go to Doctor Johaan for The Haunted Mills",
                0, 2259.04f, 347.048f, 36.1f, 6.f));
            g.steps.push_back(aq("q367_accept", "accept The Haunted Mills / darkhound quest (367)",
                367, 1518, 0, 2259.04f, 347.048f, 36.1f));

            // Q404: Dillinger (L6) — 7 Putrid Claws from Rotting Dead
            g.steps.push_back(mv("q404_go_dillinger", "go to Deathguard Dillinger",
                0, 2287.66f, 403.372f, 34.0f, 6.f));
            g.steps.push_back(aq("q404_accept", "accept Putrid Claws quest (404)",
                404, 1496, 0, 2287.66f, 403.372f, 34.0f));

            // Q374: Burgess (L7) — 10 Scarlet Insignia Rings from Scarlet Warriors
            g.steps.push_back(mv("q374_go_burgess", "go to Deathguard Burgess for Scarlet Insignia",
                0, 2270.7f, 279.998f, 35.3f, 6.f));
            g.steps.push_back(aq("q374_accept", "accept Scarlet Insignia Rings quest (374)",
                374, 1652, 0, 2270.7f, 279.998f, 35.3f));

            // Q427: Zygand (L8) — kill 10 Scarlet Warriors; pick up before grinding them
            g.steps.push_back(mv("q427_go_zygand", "go to Deathguard Zygand for Scarlet Warriors quest",
                0, 2278.08f, 295.587f, 35.3f, 6.f));
            g.steps.push_back(aq("q427_accept", "accept Scarlet Warriors quest (427)",
                427, 1515, 0, 2278.08f, 295.587f, 35.3f));

            // Kill Rotting Dead + Darkhounds in one area sweep
            g.steps.push_back(mv("q404_go_rotting", "go to Rotting Dead area for Putrid Claws",
                0, 2241.f, 621.f, 34.f, 80.f));
            g.steps.push_back(ki("q404_kill", "kill Rotting Dead for Putrid Claws (q404)",
                404, 0, 2241.f, 621.f, 34.f, 100.f));

            g.steps.push_back(mv("q367_go_darkhounds", "go to Rot Hide Darkhound area",
                0, 2200.f, 900.f, 38.f, 80.f));
            g.steps.push_back(ki("q367_kill", "kill Rot Hide Darkhounds for blood (q367)",
                367, 0, 2200.f, 900.f, 38.f, 100.f));

            // Kill Scarlet Warriors for q374 + q427 simultaneously
            g.steps.push_back(mv("q374_go_scarlets", "go to Scarlet Warrior area",
                0, 2391.f, 1564.f, 40.f, 80.f));
            g.steps.push_back(ki("q374_kill", "kill Scarlet Warriors for insignia rings (q374)",
                374, 0, 2391.f, 1564.f, 40.f, 100.f));
            g.steps.push_back(ki("q427_kill", "kill Scarlet Warriors for kill count (q427)",
                427, 0, 2391.f, 1564.f, 40.f, 100.f));

            // Turn in Brill quests
            g.steps.push_back(mv("q404_return_dillinger", "return to Deathguard Dillinger",
                0, 2287.66f, 403.372f, 34.0f, 6.f));
            g.steps.push_back(tq("q404_turnin", "turn in Putrid Claws (404)",
                404, 1496, 0, 2287.66f, 403.372f, 34.0f));

            g.steps.push_back(mv("q367_return_johaan", "return to Doctor Johaan",
                0, 2259.04f, 347.048f, 36.1f, 6.f));
            g.steps.push_back(tq("q367_turnin", "turn in darkhound quest (367)",
                367, 1518, 0, 2259.04f, 347.048f, 36.1f));

            g.steps.push_back(mv("q374_return_burgess", "return to Deathguard Burgess",
                0, 2270.7f, 279.998f, 35.3f, 6.f));
            g.steps.push_back(tq("q374_turnin", "turn in Scarlet Insignia Rings (374)",
                374, 1652, 0, 2270.7f, 279.998f, 35.3f));

            g.steps.push_back(mv("q427_return_zygand", "return to Deathguard Zygand",
                0, 2278.08f, 295.587f, 35.3f, 6.f));
            g.steps.push_back(tq("q427_turnin", "turn in Scarlet Warriors (427)",
                427, 1515, 0, 2278.08f, 295.587f, 35.3f));

            // Q370: Sevren (L9, needs q427) — kill Captain Perrine (1662) elite
            g.steps.push_back(mv("q370_go_sevren", "go to Deathguard Sevren for Captain Perrine",
                0, 2305.91f, 265.164f, 38.75f, 6.f));
            g.steps.push_back(aq("q370_accept", "accept Captain Perrine quest (370)",
                370, 1499, 0, 2305.91f, 265.164f, 38.75f));
            g.steps.push_back(mv("q370_go_perrine", "go to Captain Perrine",
                0, 1795.f, 722.f, 49.f, 20.f));
            g.steps.push_back(ki("q370_kill", "kill Captain Perrine (q370)",
                370, 0, 1795.f, 722.f, 49.f, 25.f));
            g.steps.push_back(mv("q370_return_sevren", "return to Deathguard Sevren",
                0, 2305.91f, 265.164f, 38.75f, 6.f));
            g.steps.push_back(tq("q370_turnin", "turn in Captain Perrine quest (370)",
                370, 1499, 0, 2305.91f, 265.164f, 38.75f));

            RegisterGuide(std::move(g));
        }

        LOG_INFO("module.idlebot", "[IdleBot] registered {} builtin guide(s).", _guides.size());
    }
}
