#include "IdleBotManager.h"
#include "IdleBotGuideLoader.h"
#include "IdleBotLog.h"
#include "IdleBotZoneRoute.h"
#include "IdleBotTrainers.h"
#include "SharedDefines.h"
#include <algorithm>
#include <cmath>
#include "Configuration/Config.h"
#include "Random.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include "StringFormat.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

// Config + logging headers verified in this checkout:
//   src/common/Configuration/Config.h   -> sConfigMgr->GetOption<T>(name, default)
//   src/common/Logging/Log.h            -> LOG_INFO("category", "msg {}", arg)
//
// Guide step executor (M3):
//   Bridge calls verified in IdleBotPlayerbotBridge.cpp before implementation.

namespace idlebot
{
    namespace
    {
        // Escape a string for safe inline use in a single-quoted SQL literal.
        // idlebot messages/ids are server-generated (no client input), but they
        // contain apostrophes (e.g. guide names) — double quotes + backslashes so
        // CharacterDatabase.Execute's StringFormat interpolation stays valid.
        std::string SqlEscape(std::string const& in)
        {
            std::string out;
            out.reserve(in.size() + 4);
            for (char c : in)
            {
                if (c == '\'' || c == '\\')
                    out.push_back(c);
                out.push_back(c);
            }
            return out;
        }

        bool ParseQuestObjectiveCondition(std::string const& condition, uint32_t& outQuestId, uint8_t& outObjectiveIndex)
        {
            std::string const prefix = "quest_objective_complete:";
            if (condition.rfind(prefix, 0) != 0)
                return false;

            std::string const payload = condition.substr(prefix.size());
            std::size_t const slash = payload.find('/');
            if (slash == std::string::npos)
                return false;

            try
            {
                outQuestId = static_cast<uint32_t>(std::stoul(payload.substr(0, slash)));
                uint32_t const oneBasedIndex = static_cast<uint32_t>(std::stoul(payload.substr(slash + 1)));
                if (oneBasedIndex == 0 || oneBasedIndex > QUEST_OBJECTIVES_COUNT)
                    return false;
                outObjectiveIndex = static_cast<uint8_t>(oneBasedIndex - 1);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        bool DirectoryExists(std::string const& path)
        {
            if (path.empty())
                return false;

            std::error_code ec;
            std::filesystem::path const candidate(path);
            return std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec);
        }

        std::string ResolveGuideDirectory(std::string const& configuredPath, std::string& outSource)
        {
            std::vector<std::string> candidates;
            auto addCandidate = [&candidates](std::string const& path)
            {
                if (path.empty())
                    return;

                if (std::find(candidates.begin(), candidates.end(), path) == candidates.end())
                    candidates.push_back(path);
            };

            addCandidate(configuredPath);
            addCandidate("./modules/mod-idlebot/data/guides");
            addCandidate("modules/mod-idlebot/data/guides");
            addCandidate("./data/guides");
            addCandidate("data/guides");

            for (std::string const& candidate : candidates)
            {
                if (DirectoryExists(candidate))
                {
                    outSource = candidate;
                    return candidate;
                }
            }

            outSource = configuredPath;
            return configuredPath;
        }

        std::string UtcTimestampString()
        {
            std::time_t const now = std::time(nullptr);
            std::tm tm {};
#if defined(_WIN32)
            gmtime_s(&tm, &now);
#else
            gmtime_r(&now, &tm);
#endif
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            return out.str();
        }

        std::string MakeRunId()
        {
            std::time_t const now = std::time(nullptr);
            std::tm tm {};
#if defined(_WIN32)
            gmtime_s(&tm, &now);
#else
            gmtime_r(&now, &tm);
#endif
            std::ostringstream out;
            out << std::put_time(&tm, "%Y%m%d_%H%M%S");
            return out.str();
        }
    }

    IdleBotManager* IdleBotManager::instance()
    {
        static IdleBotManager mgr;
        return &mgr;
    }

    void IdleBotManager::Initialize()
    {
        _soakRunId = MakeRunId();
        _botSessionSerial = 0;
        _enabled       = sConfigMgr->GetOption<bool>("IdleBot.Enabled", false);
        _tickMs        = sConfigMgr->GetOption<uint32_t>("IdleBot.TickMs", 1000);
        _maxActiveBots = sConfigMgr->GetOption<uint32_t>("IdleBot.MaxActiveBots", 5);
        // Step watchdog floor: a stuck step is only skipped after this much ACTIVE time
        // (offline gaps frozen, kill-progress resets it). Generous so the bot really tries
        // a quest before giving up; per-step timeout_seconds can extend but not shorten it.
        _stepSkipSeconds = sConfigMgr->GetOption<uint32_t>("IdleBot.StepSkipSeconds", 2700);
        _maxQuestFailureMinutes = sConfigMgr->GetOption<uint32_t>("IdleBot.MaxQuestFailureMinutes", 15);
        _maxDropFarmMinutes = sConfigMgr->GetOption<uint32_t>("IdleBot.MaxDropFarmMinutes", 20);
        _decisionMode  = sConfigMgr->GetOption<std::string>("IdleBot.DecisionMode", "strict");
        _accumMs       = 0;

        // Death handling (Priority 2).
        _deathEnabled            = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.Enabled", true);
        _allowDirectResurrect    = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.AllowDirectResurrect", true);
        _allowGraveyardResurrect = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.AllowGraveyardResurrect", true);
        _maxCorpseRunAttempts    = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.MaxCorpseRunAttempts", 10);
        _maxDeathsPerStep        = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.MaxDeathsPerStep", 3);
        _pauseAfterDeathLoop     = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.PauseAfterDeathLoop", true);
        _ghostStallTicks         = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.GhostStallTicks", 8);
        _noSkipBelowLevel        = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.NoSkipBelowLevel", 20);
        _rescueRelocateBelowLevel = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.RescueRelocateBelowLevel", true);
        _maxRescueRelocates      = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.MaxRescueRelocates", 5);

        // Inventory / town maintenance (Priority 5).
        _townMaintenanceEnabled  = sConfigMgr->GetOption<bool>("IdleBot.TownMaintenance.Enabled", true);
        // false (default) = player-like: run to a real merchant/repair NPC to
        // sell + repair. true = vendor-free magic maintenance (rndbot-style:
        // repair + restock anywhere, no travel) — faster but not player-like.
        _vendorFreeMaintenance   = sConfigMgr->GetOption<bool>("IdleBot.VendorFreeMaintenance", false);
        _minFreeSlotsBeforeQuest = sConfigMgr->GetOption<uint32_t>("IdleBot.Inventory.MinFreeSlotsBeforeQuest", 2);
        _minFreeSlotsBeforeGrind = sConfigMgr->GetOption<uint32_t>("IdleBot.Inventory.MinFreeSlotsBeforeGrind", 4);
        _repairBelowDurabilityPct = sConfigMgr->GetOption<uint32_t>("IdleBot.TownMaintenance.RepairBelowDurabilityPct", 40);

        // Contested gameobject handling (InteractGameObject steps).
        _gameObjectWaitForRespawn     = sConfigMgr->GetOption<bool>("IdleBot.GameObject.WaitForRespawn", true);
        _gameObjectRetryEveryMs       = sConfigMgr->GetOption<uint32_t>("IdleBot.GameObject.RetryEverySec", 5) * 1000u;
        _gameObjectRoamEveryMs        = sConfigMgr->GetOption<uint32_t>("IdleBot.GameObject.RoamEverySec", 20) * 1000u;
        _gameObjectRequiredMaxWaitMs  = sConfigMgr->GetOption<uint32_t>("IdleBot.GameObject.RequiredMaxWaitMinutes", 0) * 60000u;
        _gameObjectOptionalMaxWaitMs  = sConfigMgr->GetOption<uint32_t>("IdleBot.GameObject.OptionalMaxWaitMinutes", 15) * 60000u;
        _gameObjectDefaultSearchRadius = sConfigMgr->GetOption<float>("IdleBot.GameObject.DefaultSearchRadius", 60.f);
        _gameObjectRoamRadius         = sConfigMgr->GetOption<float>("IdleBot.GameObject.RoamRadius", 35.f);

        // Telemetry (Priority 6).
        _eventsToDb              = sConfigMgr->GetOption<bool>("IdleBot.Telemetry.Enabled", true);
        _debugEnabled            = sConfigMgr->GetOption<bool>("IdleBot.Debug.Enabled", false);

        // Playerlike policy enforcement (see docs/PLAYERLIKE_POLICY.md).
        // All cheat flags default OFF. Enabling any of them during a soak produces
        // invalid progression data and must be explicitly acknowledged in config.
        _playlikeMode            = sConfigMgr->GetOption<bool>("IdleBot.PlayerlikeMode", true);
        _allowCheatTeleport      = sConfigMgr->GetOption<bool>("IdleBot.AllowCheatTeleport", false);
        _allowCheatResurrect     = sConfigMgr->GetOption<bool>("IdleBot.AllowCheatResurrect", false);
        _allowForceQuestAdvance  = sConfigMgr->GetOption<bool>("IdleBot.AllowForceQuestAdvance", false);
        _allowForceSkipForSoak   = sConfigMgr->GetOption<bool>("IdleBot.AllowForceSkipForSoak", false);
        _allowGMRecovery         = sConfigMgr->GetOption<bool>("IdleBot.AllowGMRecovery", false);

        bool const anyCheatEnabled = _allowCheatTeleport || _allowCheatResurrect ||
            _allowForceQuestAdvance || _allowForceSkipForSoak || _allowGMRecovery || !_playlikeMode;
        if (anyCheatEnabled)
        {
            LOG_WARN("module.idlebot",
                "[IdleBot][POLICY] *** CHEAT/DEBUG FLAGS ENABLED ***  "
                "PlayerlikeMode={} CheatTeleport={} CheatResurrect={} ForceQuestAdvance={} "
                "ForceSkipForSoak={} GMRecovery={}. "
                "Progression data during this run is INVALID for playerlike assessment.",
                _playlikeMode ? 1 : 0, _allowCheatTeleport ? 1 : 0,
                _allowCheatResurrect ? 1 : 0, _allowForceQuestAdvance ? 1 : 0,
                _allowForceSkipForSoak ? 1 : 0, _allowGMRecovery ? 1 : 0);
        }

        // Adaptive combat (smart engagement modes).
        _maxPull                 = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.MaxPull", 3);
        _lowHpPct                = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.LowHpPct", 35);
        _lowManaPct              = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.LowManaPct", 20);
        _aoeThreshold            = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.AoeThreshold", 3);
        _criticalHpPct           = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.CriticalHpPct", 25);
        _restBeforePullHpPct     = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.RestBeforePullHpPct", 70);
        _restBeforePullManaPct   = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.RestBeforePullManaPct", 50);
        _restMaxTicks            = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.RestMaxTicks", 30);
        _rangedKite              = sConfigMgr->GetOption<bool>("IdleBot.Combat.RangedKite", true);
        _pullDistance            = sConfigMgr->GetOption<float>("IdleBot.Combat.PullDistance", 30.f);
        _lootRadius              = sConfigMgr->GetOption<float>("IdleBot.Combat.LootRadius", 45.f);
        _autoGear                = sConfigMgr->GetOption<bool>("IdleBot.AutoGear", false);
        _skinMobs                = sConfigMgr->GetOption<bool>("IdleBot.SkinMobs", false);
        _mailRecipient           = sConfigMgr->GetOption<std::string>("IdleBot.MailRecipient", "");
        _goSafetyClearRadius     = sConfigMgr->GetOption<float>("IdleBot.Combat.GoSafetyClearRadius", 8.f);
        _packAvoidSize           = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.PackAvoidSize", 5);
        _panicFleeSize           = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.PanicFleeSize", 6);

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
        LoadConfiguredGuides();

        // Restore the registry so bots survive a worldserver restart.
        LoadBots();

        if (!_enabled)
        {
            LOG_INFO("module.idlebot", "[IdleBot] disabled by config (IdleBot.Enabled = 0). Commands still respond.");
            return;
        }

        LOG_INFO("module.idlebot", "[IdleBot] initialized. tick={}ms maxActiveBots={} bridge={} soakRunId={}",
            _tickMs, _maxActiveBots, _bridge ? "on" : "off", _soakRunId);
    }

    void IdleBotManager::Shutdown()
    {
        // Flush guide progress + death counters so a restart resumes cleanly. Each
        // transition already persists; this captures any mid-step counter changes.
        for (auto const& [name, rec] : _bots)
            PersistProgress(rec);

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
        // One small action per permitted active bot. Never loop until "done" —
        // that would block the world thread. Each TickBot does at most one step.
        std::vector<BotRecord*> activeBots;
        activeBots.reserve(_bots.size());

        for (auto& [name, rec] : _bots)
        {
            if (!rec.active || rec.paused)
                continue;
            activeBots.push_back(&rec);
        }

        std::sort(activeBots.begin(), activeBots.end(),
            [](BotRecord const* a, BotRecord const* b)
            {
                return a->name < b->name;
            });

        std::size_t const allowedCount = std::min<std::size_t>(activeBots.size(), _maxActiveBots);
        for (std::size_t i = 0; i < activeBots.size(); ++i)
            TickBot(*activeBots[i], i < allowedCount);
    }

    void IdleBotManager::TickBot(BotRecord& rec, bool const allowRuntime)
    {
        if (!_bridge)
            return;

        if (!rec.guid)
            rec.guid = _bridge->GetBotGuid(rec.name);

        if (!allowRuntime)
        {
            BotLiveStatus live;
            bool const liveKnown = rec.guid && _bridge->GetLiveStatus(rec.guid, live);

            rec.controlWaitTicks = 0;
            rec.controlWaitArmed = false;

            if (liveKnown && live.online)
            {
                if (rec.loginRetryTicks == 0)
                {
                    LOG_INFO("module.idlebot", "[IdleBot] bot '{}': over active limit ({}), releasing to standby.",
                        rec.name, _maxActiveBots);
                    _bridge->ReleaseBot(rec.name);
                    rec.loginRetryTicks = 10;
                }
                else
                    --rec.loginRetryTicks;
            }
            else
                rec.loginRetryTicks = 0;

            return;
        }

        BotLiveStatus live;
        bool const liveKnown = rec.guid && _bridge->GetLiveStatus(rec.guid, live);
        if (!liveKnown || !live.online)
        {
            rec.controlWaitTicks = 0;
            // The step watchdog must measure ACTIVE time on the step, not wall-clock.
            // While the bot is offline (login churn under heavy load) it can't make
            // progress, so freeze the timer — otherwise a churning bot "times out" and
            // skips quests it never got a fair chance at (it blew through whole guides).
            rec.stepElapsedMs = 0;
            if (rec.loginRetryTicks == 0)
            {
                _bridge->EnsureBotOnline(rec.name);
                rec.loginRetryTicks = 10;
            }
            else
                --rec.loginRetryTicks;

            return;
        }

        // Playerbots may take a few ticks after connection before the bot session
        // is fully attached. Do not run guide logic against a visible player that
        // is not yet under playerbot control.
        if (!live.controlled)
        {
            rec.stepElapsedMs = 0;   // freeze the step watchdog while AI isn't attached
            if (rec.controlWaitTicks == 0)
            {
                if (rec.controlWaitArmed)
                {
                    // Waited a LONG time and the bot is STILL online with no
                    // playerbot AI — only now treat it as genuinely wedged and force
                    // a logout so the next EnsureBotOnline re-adds it cleanly.
                    // IMPORTANT: be patient. The AI is created by an async
                    // OnBotLoginOperation queued behind every other bot's operations;
                    // under load (1000+ bots) it can take minutes. Releasing early
                    // destroys the just-created AI AND re-queues the login at the back
                    // of the backlog, so the bot never catches up — the cause of bots
                    // stuck cycling "online, no AI". Waiting lets the queued AI attach
                    // complete; once controlled, the block below takes over.
                    LOG_WARN("module.idlebot", "[IdleBot] bot '{}': online but no playerbot AI after waiting — releasing to re-add clean.", rec.name);
                    _bridge->ReleaseBot(rec.name);
                    rec.controlWaitArmed = false;
                    rec.loginRetryTicks = 0;   // allow immediate re-add next tick
                }
                else
                {
                    LOG_WARN("module.idlebot", "[IdleBot] bot '{}': online but not under playerbot control yet; waiting for async AI attach.", rec.name);
                    rec.controlWaitArmed = true;
                }
                rec.controlWaitTicks = 300;   // ~5 min/cycle (was 30): wait out the async AI-attach backlog before releasing
            }
            else
                --rec.controlWaitTicks;
            return;
        }

        rec.loginRetryTicks = 0;
        rec.controlWaitTicks = 0;
        rec.controlWaitArmed = false;
        ++rec.globalTick;

        // Write live state snapshot to DB every 2 ticks (~2 s) for the dashboard.
        if (rec.globalTick % 2 == 1)
            WriteLiveState(rec);

        // One-time per-session setup (ensure looting strategy is on).
        EnsureStrategies(rec);

        // Death recovery overrides everything (Pitfall D). HandleDeath returns true
        // while the bot is dead/recovering — skip the rest of the tick so we never
        // issue guide movement/combat actions that fight the dead-state AI.
        if (HandleDeath(rec))
            return;

        // Resurrection sickness (spell 15007): wait it out instead of questing
        // with -75% stats and dying immediately. Sit and recover.
        if (_bridge->HasResSickness(rec.guid))
        {
            _bridge->Recover(rec.guid);
            return;
        }

        // Emit IdleRPG events from polled deltas (level/quest/loot/inventory).
        PollDeltas(rec);

        // Learn new spells + talents on level-up. Without this the bot fights
        // with L1 spells forever (InitClassSpells only runs at login).
        {
            uint32_t botLevel = _bridge->GetLevel(rec.guid);
            if (botLevel > rec.lastTrainedLevel)
            {
                _bridge->LearnAvailableSpells(rec.guid);
                rec.lastTrainedLevel = botLevel;
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': learned new spells for level {}.",
                    rec.name, botLevel);
            }
            if (botLevel > rec.lastSpeccedLevel)
            {
                _bridge->AutoSpecTalents(rec.guid);
                rec.lastSpeccedLevel = botLevel;
            }
        }

        // Organic mode: hand quest pickup / travel / combat to playerbots'
        // autonomous AI and supervise only. Bypasses the guide-step executor.
        if (rec.decisionMode == "organic")
        {
            TickOrganic(rec);
            return;
        }

        // M3: guide step executor. One step at a time; never advance more than
        // one step per tick so the world thread isn't held up.
        if (rec.guideId.empty())
        {
            if (rec.noGuideTicks == 0)
            {
                LOG_WARN("module.idlebot", "[IdleBot] bot '{}': no guide assigned — bot idle until one is set.", rec.name);
                EmitEvent(rec, "GUIDE", "no guide assigned — waiting");
            }
            else if (rec.noGuideTicks % 30 == 0)
                LOG_WARN("module.idlebot", "[IdleBot] bot '{}': still no guide assigned ({}+ ticks idle).",
                    rec.name, rec.noGuideTicks);
            ++rec.noGuideTicks;
            return;
        }
        rec.noGuideTicks = 0;

        auto git = _guides.find(rec.guideId);
        if (git == _guides.end())
        {
            LOG_WARN("module.idlebot", "[IdleBot] bot '{}': guide '{}' not found in {} loaded guides — clearing.",
                rec.name, rec.guideId, _guides.size());
            rec.guideId.clear();
            rec.currentStepIndex = 0;
            ResetObjectStepState(rec);
            PersistProgress(rec);
            return;
        }

        const Guide& guide = git->second;
        if (rec.currentStepIndex < guide.steps.size())
        {
            FastForwardRewardedSteps(rec, guide, "tick");
        }
        if (rec.currentStepIndex >= guide.steps.size())
        {
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': guide '{}' complete!", rec.name, rec.guideId);
            EmitEvent(rec, "GUIDE", Acore::StringFormat("guide '{}' complete", rec.guideId));

            // Chain-load the next guide if one is specified.
            if (!guide.nextGuide.empty() && _guides.count(guide.nextGuide))
            {
                rec.guideId = guide.nextGuide;
                rec.currentStepIndex = 0;
                rec.stepState = "idle";
                ResetObjectStepState(rec);
                FastForwardRewardedSteps(rec, _guides[rec.guideId], "chain-load");
                PersistProgress(rec);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': chain-loading next guide '{}'.",
                    rec.name, rec.guideId);
                EmitEvent(rec, "GUIDE", Acore::StringFormat(
                    "chain-loaded '{}'", rec.guideId));
                return;
            }

            rec.guideId.clear();
            rec.currentStepIndex = 0;
            rec.stepState = "idle";
            ResetObjectStepState(rec);
            PersistProgress(rec);
            return;
        }

        const GuideStep& step = guide.steps[rec.currentStepIndex];

        if (!StepAppliesToBot(rec, step))
        {
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': skipping step {}/{} '{}' due to race/class/faction restriction.",
                rec.name, rec.currentStepIndex + 1, guide.steps.size(), step.name);
            EmitEvent(rec, "GUIDE", Acore::StringFormat("skipped restricted step '{}' (step {}/{})",
                step.name, rec.currentStepIndex + 1, guide.steps.size()));
            AdvanceStep(rec);
            return;
        }

        // Rescue relocate (set by HandleDeath / watchdog for a low-level bot we refuse
        // to let skip): the death/stuck loop is almost always positional — the bot
        // drifted into over-level mobs far from where the step expects it. Teleport it
        // back to the step anchor so it re-approaches the SAME quest from the right
        // place, instead of abandoning a starter quest. Done here (alive), not in
        // HandleDeath, so the teleport doesn't fight the dead/ghost state.
        if (rec.rescueRelocateRequested)
        {
            bool const hasAnchor = !(step.coords.x == 0.f && step.coords.y == 0.f);
            // Walk the bot back toward the step anchor using normal navmesh pathing
            // (MoveTo, not TeleportBot). Player-like movement; no GM shortcuts.
            if (hasAnchor && !_bridge->IsInCombat(rec.guid))
            {
                _bridge->MoveTo(rec.guid, step.coords.mapId,
                    step.coords.x, step.coords.y, step.coords.z, 5.f);
                rec.rescueRelocateRequested = false;
                rec.deathCountStep = 0;
                rec.stepElapsedMs = 0;
                rec.stuckTicks = 0;
                EmitEvent(rec, "RECOVERY", Acore::StringFormat(
                    "walking back to quest area (step {}) — rescue relocate {}/{}",
                    rec.currentStepIndex + 1, rec.rescueRelocateCount, _maxRescueRelocates));
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': rescue-relocate walking to step {} anchor (map {} {:.0f},{:.0f}). "
                    "Attempt {}/{}.",
                    rec.name, rec.currentStepIndex + 1, step.coords.mapId,
                    step.coords.x, step.coords.y,
                    rec.rescueRelocateCount, _maxRescueRelocates);
                PersistProgress(rec);
            }
            else if (!hasAnchor)
            {
                // No usable anchor to relocate to — just drop the request and let the
                // bot keep trying from where it is (still not skipping below the floor).
                rec.rescueRelocateRequested = false;
            }
            return;
        }

        // Death-loop flag (legacy persistent state or race): quarantine, never skip.
        if (rec.skipQuestRequested)
        {
            rec.skipQuestRequested = false;
            rec.deathCountStep = 0;
            BlockBot(rec, "COMBAT_TOO_HARD", Acore::StringFormat(
                "[FAILURE:COMBAT_TOO_HARD] died too many times on step {} (quest {}) — "
                "quarantining. Fix combat/pull/guide and use '.idlebot resume {}' to continue.",
                rec.currentStepIndex, step.questId.value_or(0), rec.name));
            LOG_WARN("module.idlebot",
                "[IdleBot] bot '{}': QUARANTINED — COMBAT_TOO_HARD on step {} (quest {}). "
                "Fix and resume.",
                rec.name, rec.currentStepIndex, step.questId.value_or(0));
            return;
        }

        // Level check: if the quest requires a higher level, grind nearby mobs
        // to catch up instead of dying on content we can't handle.
        if (step.questId.has_value())
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(*step.questId);
            uint32_t botLevel = _bridge->GetLevel(rec.guid);
            if (quest && quest->GetMinLevel() > botLevel)
            {
                // Grind nearby low-level mobs to gain XP and level up.
                CombatContext cc;
                _bridge->GetCombatContext(rec.guid, cc);
                if (cc.valid && !cc.inCombat)
                {
                    BotPosition hostilePos;
                    uint64_t hostileGuid = 0;
                    if (_bridge->FindNearestHostile(rec.guid, 40.f, hostilePos, hostileGuid) &&
                        hostileGuid != 0)
                    {
                        uint32_t mobLevel = _bridge->GetCreatureLevel(rec.guid, hostileGuid);
                        if (mobLevel > 0 && mobLevel <= botLevel + 2)
                            _bridge->AttackCreature(rec.guid, hostileGuid);
                        else
                            RoamKillObjective(rec, step);
                    }
                    else
                        RoamKillObjective(rec, step);
                }
                rec.stepElapsedMs += _tickMs;
                if (rec.stepElapsedMs % 30000 < _tickMs)
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': grinding to level {} for quest {} (currently L{}).",
                        rec.name, quest->GetMinLevel(), *step.questId, botLevel);
                // Under-leveled 10+ min — guide step placed too early; quarantine, never skip.
                if (rec.stepElapsedMs > 600000)
                {
                    rec.paused = true;
                    EmitEvent(rec, "FAILURE", Acore::StringFormat(
                        "[FAILURE:GRIND_NO_PROGRESS] grinding for level {} (quest {}) — "
                        "no level-up after 10 min. Guide step placed too early. "
                        "Fix guide and use '.idlebot resume {}' to continue.",
                        quest->GetMinLevel(), *step.questId, rec.name));
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': QUARANTINED — GRIND_NO_PROGRESS for quest {} "
                        "after 10 min under-level grind. Fix guide and resume.",
                        rec.name, *step.questId);
                    PersistProgress(rec);
                }
                return;
            }
        }

        // Step elapsed tracking (for objective progress reset).
        rec.stepElapsedMs += _tickMs;

        // Bag-full / durability guard before quest/grind/gameobject steps (Pitfall E).
        // If maintenance is being handled this tick, consume it and try again next.
        if (MaintenanceGuard(rec))
            return;

        // Cross-continent transport: if the step is on a different map, run the
        // transport state machine (teleport to dock → wait → board → ride → disembark)
        // instead of normal step execution. Resumes normal execution once the bot
        // arrives on the correct map.
        if (TickTransport(rec, step))
            return;

        // Class-specific travel: handle steps that require a class teleport spell
        // (e.g. druid Teleport: Moonglade) to reach a zone that normal pathfinding
        // cannot cross. Must run after TickTransport (same-map class travel) but
        // before the step executor. Returns true while transit is in progress.
        if (TickClassTravel(rec, step))
            return;

        // Position-stall detection: if the bot hasn't moved for ~5s, try escalating
        // unstick actions (jump → strafe → reverse → teleport to anchor).
        if (TickUnstick(rec))
            return;

        if (rec.stepState != "running")
        {
            rec.stepState = "running";
            PersistProgress(rec);
        }

        // Delegate unguided kill steps to playerbots' "grind" strategy. Guided
        // kill steps have an explicit creature list, so idlebot owns target choice
        // and leaves autonomous "attack anything" off to avoid ambient mobs.
        bool const wantGrind = (step.type == StepType::KillMobs && step.creatureIds.empty());
        if (wantGrind != rec.grindOn)
        {
            if (wantGrind)
            {
                // grind = "attack anything when no target"; loot out-prioritises it
                // so the bot loots each kill before engaging the next. Need both.
                _bridge->SetNonCombatStrategy(rec.guid, "+grind");
                _bridge->SetNonCombatStrategy(rec.guid, "+loot");
            }
            else
            {
                _bridge->SetNonCombatStrategy(rec.guid, "-grind");
            }
            rec.grindOn = wantGrind;
        }
        if (step.type == StepType::KillMobs)
            _bridge->SetNonCombatStrategy(rec.guid, "+loot");

        bool stepDone = false;

        // Combat awareness on non-kill steps. On travel/accept/turn-in steps,
        // DON'T stop to fight random aggro — keep moving to the destination.
        // Only fight if hp is critically low (can't outrun). On interact steps
        // (escort, gameobject), fight normally since we need to stay in the area.
        if (step.type != StepType::KillMobs)
        {
            CombatContext cc;
            _bridge->GetCombatContext(rec.guid, cc);
            if (cc.valid)
            {
                char const* rmode = nullptr;
                bool const engaged = cc.inCombat || cc.myAttackers > 0 || _bridge->IsInCombat(rec.guid);
                bool const travelStep = (step.type == StepType::AcceptQuest ||
                    step.type == StepType::TurnInQuest || step.type == StepType::MoveTo ||
                    step.type == StepType::TaxiRide);

                if (!engaged)
                {
                    rec.combatStallTicks = 0;
                    rec.lastCombatHpPct = -1.f;
                }
                if (engaged && travelStep && cc.hpPct > 30.f)
                {
                    // On travel: DON'T consume this tick. Fall through to the step
                    // executor so MoveToStepPosition keeps running. The bot keeps
                    // walking to its destination while the mob hits it. The mob
                    // leashes after ~40yd. Only stop to fight if HP critically low.
                    // (Don't set rmode — let it fall through.)
                }
                else if (engaged)
                {
                    // On interact/escort steps or critically low HP: fight back.
                    rec.lootGraceTicks = 18;
                    bool const wantAoe = cc.aoeCount >= _aoeThreshold;
                    if (wantAoe != rec.aoeOn)
                    {
                        _bridge->SetCombatStrategy(rec.guid, wantAoe ? "+aoe" : "-aoe");
                        rec.aoeOn = wantAoe;
                    }
                    // Frozen-AI breaker. A bot can sit flagged "in combat" with a target
                    // it never swings at (class AI stalled: target acquired, rotation
                    // never fires) and freeze indefinitely, blocking a travel/interact
                    // step. Detect the freeze by HP STAGNATION: in a real fight hp moves
                    // (we take hits or the mob dies and a new one engages); if hp is flat
                    // while still "engaged", nothing is actually happening. (myAttackers is
                    // unreliable — it reads 0 even mid-fight.) On a flat-hp stall, re-assert
                    // the attack to kick the rotation; after a long stall stop deferring so
                    // the step's own action resumes and the bot walks out of the phantom
                    // combat instead of freezing here.
                    bool const hpMoved = rec.lastCombatHpPct < 0.f ||
                                         std::fabs(cc.hpPct - rec.lastCombatHpPct) > 2.0f;
                    rec.lastCombatHpPct = cc.hpPct;
                    if (hpMoved)
                    {
                        rec.combatStallTicks = 0;
                    }
                    else
                    {
                        ++rec.combatStallTicks;
                        if (cc.currentTargetEntry == 0 || rec.combatStallTicks % 4 == 0)
                        {
                            BotPosition hp;
                            uint64_t hg = 0;
                            if (_bridge->FindNearestHostile(rec.guid, 40.f, hp, hg) && hg != 0)
                                _bridge->AttackCreature(rec.guid, hg);
                        }
                    }

                    if (rec.combatStallTicks < 16)
                        rmode = "defend";
                }
                else if (cc.hpPct < static_cast<float>(_lowHpPct) ||
                         cc.manaPct < static_cast<float>(_lowManaPct))
                {
                    // Safe (not engaged) but hurt/low mana — eat/drink before moving
                    // on. Never reached while in combat (the defend branch wins), so
                    // we don't stand still trying to eat while a mob beats on us.
                    _bridge->Recover(rec.guid);
                    rmode = "recover";
                }
                else if (rec.lootGraceTicks > 0)
                {
                    // Just cleared a fight during travel — switch to non-combat engine so
                    // LootNonCombatStrategy fires, then count down. LootNearby just
                    // reports; the actual looting is the playerbots non-combat strategy.
                    if (rec.lootGraceTicks == 18)
                        _bridge->BeginLoot(rec.guid, rec.lastEngagedGuid);
                    LootAttempt const la = _bridge->LootNearby(rec.guid);
                    if (!la.hasLoot)
                        --rec.lootGraceTicks;
                    rmode = "loot";
                }

                if (rmode)
                {
                    ++rec.reactivePinTicks;
                    if (_debugEnabled && (rec.dbgThrottle++ % 3 == 0))
                    {
                        BotPosition pos = _bridge->GetPosition(rec.guid);
                        LOG_INFO("module.idlebot",
                            "[IdleBot][dbg] {} step{} reactive={}: hp={:.0f}% attackers={} target={}:{}@{:.1f} pos=({:.0f},{:.0f})",
                            rec.name, rec.currentStepIndex, rmode, cc.hpPct, cc.myAttackers,
                            cc.currentTargetEntry, cc.currentTargetName, cc.currentTargetDistance,
                            pos.valid ? pos.x : 0.f, pos.valid ? pos.y : 0.f);
                    }
                    // Anti-pin: incidental combat/loot must NOT defer a travel/accept step
                    // forever. A mob-dense area can keep flickering "engaged"/loot-grace so
                    // this block always returns and the bot never walks to its giver/target
                    // (seen: a bot pinned 45 min looping reactive=loot). After a long defer,
                    // push on toward the objective — the class AI keeps fighting while we
                    // MoveTo, so we clear/aggro on the way instead of standing still.
                    if (rec.reactivePinTicks < 30)
                        return;  // defer this step's normal action until we're clear
                    // else fall through to the step action (resume travel toward the goal)
                }
                else
                {
                    rec.reactivePinTicks = 0;
                }
            }
        }

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
                    if (step.areaTrigger.has_value())
                    {
                        _bridge->FireAreaTrigger(rec.guid, *step.areaTrigger);
                        LOG_INFO("module.idlebot",
                            "[IdleBot] bot '{}': at move_to destination — firing AreaTrigger {}.",
                            rec.name, *step.areaTrigger);
                    }
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

            // 30-yard approach: matches TurnInQuest — direct AddQuestAndCheckCompletion
            // doesn't require close proximity.
            if (MoveToStepPosition(rec, step, 30.0f))
                break;

            if (_bridge->IsMounted(rec.guid))
                _bridge->Dismount(rec.guid);

            uint32_t qid = *step.questId;
            QuestState qs = _bridge->GetQuestStatus(rec.guid, qid);
            if (qs != QuestState::NotStarted && qs != QuestState::Unknown)
            {
                stepDone = true;  // already accepted (or rewarded)
                rec.stuckTicks = 0;
                break;
            }
            uint32_t entry = step.npcId.value_or(0);
            // Try to interact with the NPC first (opens gossip/quest frame),
            // then accept the quest — handles quests behind gossip dialogs (#50).
            if (entry != 0)
            {
                uint64_t npcGuid = _bridge->FindNearestCreatureEntry(rec.guid, entry, 15.f);
                if (npcGuid != 0)
                    _bridge->InteractWithNpc(rec.guid, npcGuid);
            }
            bool const accepted = _bridge->AcceptQuest(rec.guid, qid, entry);
            if (!accepted)
            {
                // Log only on first failure to avoid per-tick spam.
                if (rec.stuckTicks == 0)
                {
                    Quest const* q = sObjectMgr->GetQuestTemplate(qid);
                    if (q && q->GetSrcItemId() != 0 &&
                        _bridge->GetItemCount(rec.guid, q->GetSrcItemId(), false) == 0)
                    {
                        LOG_WARN("module.idlebot",
                            "[IdleBot] bot '{}': accept quest {} — missing start item {}.",
                            rec.name, qid, q->GetSrcItemId());
                    }
                    else
                    {
                        LOG_INFO("module.idlebot",
                            "[IdleBot] bot '{}': accept quest {} failed (prereqs not met).",
                            rec.name, qid);
                    }
                }
                if (++rec.stuckTicks > 30)
                {
                    BlockBot(rec, "QUEST_ACCEPT_FAILED", Acore::StringFormat(
                        "[FAILURE:QUEST_ACCEPT_FAILED] cannot accept quest {} after {} ticks. "
                        "Check NPC/prereqs/guide. Use '.idlebot resume {}' after fixing.",
                        qid, rec.stuckTicks, rec.name));
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': QUARANTINED — QUEST_ACCEPT_FAILED for quest {} "
                        "after {} ticks. Check NPC/prereqs/guide.",
                        rec.name, qid, rec.stuckTicks);
                    rec.stuckTicks = 0;
                    return;
                }
                break;
            }
            rec.stuckTicks = 0;

            // Look-ahead: accept all quests at this hub before leaving.
            // If the next steps are also accept_quest at a nearby NPC, accept
            // them now so the bot picks up the whole batch in one visit.
            if (_bridge->GetQuestStatus(rec.guid, qid) == QuestState::InProgress)
            {
                BotPosition pos = _bridge->GetPosition(rec.guid);
                for (uint32_t lookahead = rec.currentStepIndex + 1;
                     lookahead < guide.steps.size() && lookahead < rec.currentStepIndex + 10;
                     ++lookahead)
                {
                    auto const& next = guide.steps[lookahead];
                    if (next.type != StepType::AcceptQuest || !next.questId.has_value())
                        break;
                    if (!StepAppliesToBot(rec, next))
                        continue;
                    // Only batch if the NPC is nearby (same hub).
                    float dx = next.coords.x - pos.x, dy = next.coords.y - pos.y;
                    if ((dx * dx + dy * dy) > 100.f * 100.f)
                        break;
                    uint32_t nextEntry = next.npcId.value_or(0);
                    _bridge->AcceptQuest(rec.guid, *next.questId, nextEntry);
                    if (_bridge->GetQuestStatus(rec.guid, *next.questId) == QuestState::InProgress)
                    {
                        LOG_INFO("module.idlebot",
                            "[IdleBot] bot '{}': batch-accepted quest {} at hub.",
                            rec.name, *next.questId);
                    }
                }
            }
            break;
        }

        case StepType::TurnInQuest:
        {
            if (!step.questId.has_value())
                { stepDone = true; break; }

            uint32_t qid = *step.questId;
            QuestState qs = _bridge->GetQuestStatus(rec.guid, qid);
            if ((qs == QuestState::NotStarted || qs == QuestState::Unknown) &&
                RewindToQuestAcceptStep(rec, guide, qid, "quest missing at turn-in"))
                return;

            if (qs == QuestState::Rewarded)
            {
                stepDone = true;
                break;
            }
            if (qs != QuestState::Complete)
            {
                // Accepted but objectives not actually done. Rather than relocate to
                // the ender forever (we never abandon below the level floor), rewind
                // to the quest's objective steps and finish them. BUT bound the
                // rewinds: a quest whose represented objective steps all complete yet
                // stays InProgress has an objective with NO guide step (e.g. an item
                // with no dropper, like q375's item 2320) — structurally undoable, so
                // retrying can't help. After a couple of bounce-backs, skip it (the
                // one case we DO abandon below the level floor, since waiting is futile).
                if (qs == QuestState::InProgress)
                {
                    // Diagnostic: log each objective's current/required counter so we can tell
                    // which kill credit is missing across soak runs.
                    for (uint8_t oi = 0; oi < 4; ++oi)
                    {
                        uint32_t cur = 0, req = 0;
                        if (_bridge->GetQuestObjectiveProgress(rec.guid, qid, oi, cur, req) && req > 0)
                            LOG_WARN("module.idlebot",
                                "[IdleBot] bot '{}': quest {} InProgress at turn-in obj[{}] {}/{}",
                                rec.name, qid, oi, cur, req);
                    }
                    if (rec.turninRewindQuestId != qid)
                    {
                        rec.turninRewindQuestId = qid;
                        rec.turninRewindCount = 0;
                    }
                    if (rec.turninRewindCount < 2)
                    {
                        bool const rewound = RewindToQuestObjectives(rec, guide, qid, "incomplete at turn-in");
                        ++rec.turninRewindCount;  // count even if no step found (discover quests)
                        if (rewound)
                            return;
                        // No objective step in guide (e.g. "explore" auto-discover quests).
                        // Fall through to log and skip on the second try.
                        LOG_WARN("module.idlebot",
                            "[IdleBot] bot '{}': quest {} incomplete at turn-in — no objective step "
                            "found (try {}/2).", rec.name, qid, rec.turninRewindCount);
                    }
                    if (rec.turninRewindCount >= 2)
                    {
                        BlockBot(rec, "QUEST_INCOMPLETE_AT_TURNIN", Acore::StringFormat(
                            "[FAILURE:QUEST_INCOMPLETE_AT_TURNIN] quest {} incomplete at turn-in "
                            "after {} rewinds — objective has no guide step. "
                            "Add missing objective step to guide. Use '.idlebot resume {}' after fixing.",
                            qid, rec.turninRewindCount, rec.name));
                        LOG_WARN("module.idlebot",
                            "[IdleBot] bot '{}': QUARANTINED — QUEST_INCOMPLETE_AT_TURNIN for quest {}. "
                            "Missing objective step in guide. Fix guide and resume.",
                            rec.name, qid);
                        rec.turninRewindQuestId = 0;
                        rec.turninRewindCount = 0;
                        return;
                    }
                }
                break;  // not yet ready to turn in
            }

            // 30-yard approach: bots often can't navigate into buildings; stop outside
            // and call RewardQuest directly (no engine-level proximity check needed).
            if (MoveToStepPosition(rec, step, 30.0f))
                break;

            if (_bridge->IsMounted(rec.guid))
                _bridge->Dismount(rec.guid);

            uint32_t entry = step.npcId.value_or(0);
            _bridge->TurnInQuest(rec.guid, qid, entry);

            // Look-ahead: turn in all completed quests at this hub.
            if (_bridge->GetQuestStatus(rec.guid, qid) == QuestState::Rewarded)
            {
                BotPosition pos = _bridge->GetPosition(rec.guid);
                for (uint32_t lookahead = rec.currentStepIndex + 1;
                     lookahead < guide.steps.size() && lookahead < rec.currentStepIndex + 10;
                     ++lookahead)
                {
                    auto const& next = guide.steps[lookahead];
                    if (next.type == StepType::AcceptQuest)
                        continue;  // skip accept steps in the batch
                    if (next.type != StepType::TurnInQuest || !next.questId.has_value())
                        break;
                    QuestState nqs = _bridge->GetQuestStatus(rec.guid, *next.questId);
                    if (nqs != QuestState::Complete)
                        continue;
                    float dx = next.coords.x - pos.x, dy = next.coords.y - pos.y;
                    if ((dx * dx + dy * dy) > 100.f * 100.f)
                        break;
                    uint32_t nextEntry = next.npcId.value_or(0);
                    _bridge->TurnInQuest(rec.guid, *next.questId, nextEntry);
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': batch-turned-in quest {} at hub.",
                        rec.name, *next.questId);
                }
            }
            break;
        }

        case StepType::KillMobs:
        {
            if (!step.questId.has_value())
            {
                stepDone = true;
                break;
            }

            if (RewindToQuestAcceptStep(rec, guide, *step.questId, "quest missing at objective"))
                return;

            uint32_t objectiveCurrent = 0;
            uint32_t objectiveRequired = 0;
            stepDone = CompletionConditionMet(rec, step, &objectiveCurrent, &objectiveRequired);

            // Progress resets the step watchdog: as long as the kill count keeps
            // climbing the bot is making real progress, so a long-but-legitimate
            // objective (kill 12 scattered mobs) is never skipped. The watchdog only
            // fires when the bot is online+controlled yet makes NO progress for 5 min.
            if (objectiveCurrent > rec.lastObjectiveCurrent)
            {
                rec.lastObjectiveCurrent = objectiveCurrent;
                rec.stepElapsedMs = 0;
            }

            BotPosition objectivePos = _bridge->GetPosition(rec.guid);
            float objectiveDistance = -1.f;
            bool const inObjectiveArea = IsInsideObjectiveArea(step, objectivePos, &objectiveDistance);
            bool const isDropFarmObjective =
                step.itemId.has_value() || step.type == StepType::CollectItems;
            uint32_t const failureTimeoutMs = std::max(
                step.timeoutSeconds,
                (isDropFarmObjective ? _maxDropFarmMinutes : _maxQuestFailureMinutes) * 60u
            ) * 1000u;

            // No-progress watchdog only applies once we're actually in the objective area.
            // While still traveling, classify hard stalls as path failures instead.
            if (!stepDone && rec.stepElapsedMs > 0 && !inObjectiveArea)
            {
                if (rec.stepElapsedMs > 300000 && rec.posStallTicks > 20)
                {
                    BlockBot(rec, "PATH_FAILED_TO_OBJECTIVE", Acore::StringFormat(
                        "[FAILURE:PATH_FAILED_TO_OBJECTIVE] kill step '{}' (quest {}) — "
                        "bot stayed {:.0f} yd from the objective area and movement stalled. "
                        "Fix travel/pathing/objective routing and use '.idlebot resume {}' to continue.",
                        step.name, step.questId.value_or(0),
                        objectiveDistance >= 0.f ? objectiveDistance : 0.f, rec.name));
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': BLOCKED — PATH_FAILED_TO_OBJECTIVE on '{}' "
                        "(quest {}). dist={:.0f} stallTicks={}.",
                        rec.name, step.name, step.questId.value_or(0),
                        objectiveDistance >= 0.f ? objectiveDistance : 0.f, rec.posStallTicks);
                    break;
                }
            }
            else if (!stepDone && rec.stepElapsedMs > 0)
            {
                if (objectiveCurrent == 0)
                {
                    if (rec.stepElapsedMs % 300000 < static_cast<uint64_t>(_tickMs))
                        LOG_WARN("module.idlebot",
                            "[IdleBot] bot '{}': kill step '{}' — zero progress for {:.0f} min inside objective area.",
                            rec.name, step.name, rec.stepElapsedMs / 60000.0);
                    if (rec.stepElapsedMs > failureTimeoutMs)
                    {
                        BlockBot(rec, "QUEST_OBJECTIVE_NO_PROGRESS", Acore::StringFormat(
                            "[FAILURE:QUEST_OBJECTIVE_NO_PROGRESS] kill step '{}' (quest {}) — "
                            "zero progress after {:.0f} min inside the objective area. "
                            "Wrong mob/item source, missing spawn cluster, or loot/progress bug. "
                            "Fix the root cause and use '.idlebot resume {}' to continue.",
                            step.name, step.questId.value_or(0), failureTimeoutMs / 60000.0, rec.name));
                        LOG_WARN("module.idlebot",
                            "[IdleBot] bot '{}': BLOCKED — QUEST_OBJECTIVE_NO_PROGRESS on '{}' "
                            "(quest {}) inside objective area after {:.0f} min.",
                            rec.name, step.name, step.questId.value_or(0), failureTimeoutMs / 60000.0);
                        break;
                    }
                }
                else if (rec.stepElapsedMs > failureTimeoutMs)
                {
                    BlockBot(rec, "QUEST_PROGRESS_STALLED", Acore::StringFormat(
                        "[FAILURE:QUEST_PROGRESS_STALLED] kill step '{}' (quest {}) — "
                        "objective progress stalled at {}/{} for {:.0f} min. "
                        "Fix target selection, loot, or progress tracking and use '.idlebot resume {}' to continue.",
                        step.name, step.questId.value_or(0), objectiveCurrent, objectiveRequired,
                        failureTimeoutMs / 60000.0, rec.name));
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': BLOCKED — QUEST_PROGRESS_STALLED on '{}' "
                        "(quest {}) at {}/{} after {:.0f} min.",
                        rec.name, step.name, step.questId.value_or(0), objectiveCurrent, objectiveRequired,
                        failureTimeoutMs / 60000.0);
                    break;
                }
            }

            if (!stepDone)
            {
                // ---- adaptive engagement mode machine ----
                // idlebot picks the MODE from context; playerbots runs the per-class
                // rotation. Modes: recover / fight / loot / roam / engage.
                CombatContext cc;
                _bridge->GetCombatContext(rec.guid, cc);
                char const* mode;

                // Per-class combat parameter overrides (set once in EnsureStrategies).
                // Fall back to global config when the class config is zero (default/unset).
                uint32_t const effectiveAoeThreshold  = rec.classConfig.aoeThreshold > 0
                    ? rec.classConfig.aoeThreshold : _aoeThreshold;
                uint32_t const effectiveRestManaPct   = rec.classConfig.restBeforePullManaPct > 0
                    ? rec.classConfig.restBeforePullManaPct : _restBeforePullManaPct;
                uint32_t const effectiveRestHpPct     = rec.classConfig.restBeforePullHpPct > 0
                    ? rec.classConfig.restBeforePullHpPct : _restBeforePullHpPct;
                uint32_t const effectiveMaxPull       = rec.classConfig.maxPullOverride > 0
                    ? rec.classConfig.maxPullOverride : _maxPull;

                bool const engaged = cc.inCombat || cc.myAttackers > 0 || _bridge->IsInCombat(rec.guid);
                if (!engaged)
                {
                    rec.combatStallTicks = 0;
                    rec.lastCombatHpPct = -1.f;
                    // Recovered to pull-ready → allow a fresh rest cycle next time.
                    if (cc.valid && cc.hpPct >= static_cast<float>(effectiveRestHpPct) &&
                        (rec.classConfig.skipManaRecover ||
                         cc.manaPct >= static_cast<float>(effectiveRestManaPct)))
                        rec.restTicks = 0;
                }
                // RETREAT — flee a losing fight: either CRITICAL hp (even a 1v1 going
                // badly — don't fight to the death) or hurt-AND-swarmed (a whole camp/cave).
                // Back off toward open ground (away from the densest hostiles), fighting on
                // the way out; let mobs leash / hp regen, then re-approach. Player-like:
                // clear and pull, don't headstrong the boss.
                bool const critical   = cc.valid && cc.hpPct < static_cast<float>(_criticalHpPct);
                bool const swarmed    = cc.valid && cc.hpPct < 40.f && cc.aoeCount >= 4;
                // mob_flood: actually being attacked by _panicFleeSize+ mobs simultaneously.
                // Use myAttackers (not aoeCount): aoeCount counts all nearby hostiles regardless
                // of whether they're attacking, so in dense starting zones (59+ wolves nearby)
                // aoeCount is always 6+ and the bot retreats forever without killing anything.
                bool const mob_flood  = cc.valid && cc.myAttackers >= _panicFleeSize;
                bool const overwhelmed = critical || swarmed || mob_flood;
                if (rec.retreatTicks > 0 || overwhelmed)
                {
                    mode = "retreat";
                    if (rec.retreatTicks == 0)
                        rec.retreatTicks = 12;          // ~12s pull-out
                    BotPosition bp = _bridge->GetPosition(rec.guid);
                    BotPosition hpos;
                    uint64_t hg = 0;
                    if (bp.valid && _bridge->FindNearestHostile(rec.guid, 40.f, hpos, hg) &&
                        hpos.valid && hpos.mapId == bp.mapId)
                    {
                        float dx = bp.x - hpos.x, dy = bp.y - hpos.y;
                        float d = std::hypot(dx, dy);
                        if (d < 1.f) { dx = 1.f; dy = 0.f; d = 1.f; }
                        _bridge->MoveTo(rec.guid, bp.mapId, bp.x + dx / d * 40.f,
                                        bp.y + dy / d * 40.f, bp.z, 5.f);
                    }
                    if (rec.retreatTicks > 0)
                        --rec.retreatTicks;
                    // End the retreat once hp has recovered enough to re-engage. Don't end
                    // early just because aoe<4 — a critical 1v1 flee must keep backing off
                    // until hp comes back (the 12-tick timer bounds it otherwise).
                    if (cc.hpPct > 65.f)
                        rec.retreatTicks = 0;
                    rec.stuckTicks = 0;
                }
                else if (cc.valid && !engaged &&
                    (cc.hpPct < static_cast<float>(_lowHpPct) ||
                     (!rec.classConfig.skipManaRecover &&
                      cc.manaPct < static_cast<float>(_lowManaPct))))
                {
                    // RECOVER — eat/drink when hurt or low mana, but only while SAFE.
                    // Never sit eating mid-fight (you can't, and you'd just take hits);
                    // in combat the FIGHT branch wins and the class AI handles survival.
                    // Warriors/rogues/hunters skip the mana-recover check (energy/rage).
                    mode = "recover";
                    _bridge->Recover(rec.guid);
                    rec.stuckTicks = 0;
                }
                else if (engaged)
                {
                    // FIGHT — let the class rotation work; hold position; arm loot-grace.
                    // Switch AoE on/off by cluster size (tracked to avoid strategy spam).
                    mode = "fight";
                    rec.lootGraceTicks = 18;
                    rec.stuckTicks = 0;
                    rec.restTicks = 0;   // pulled successfully → rest cycle consumed
                    bool const wantAoe = cc.aoeCount >= effectiveAoeThreshold;
                    if (wantAoe != rec.aoeOn)
                    {
                        _bridge->SetCombatStrategy(rec.guid, wantAoe ? "+aoe" : "-aoe");
                        rec.aoeOn = wantAoe;
                    }
                    // CLEAR ADDS — when swarmed and the class AI is tunneling a distant
                    // (or no) target while other mobs beat on the bot, force-switch to the
                    // nearest threat so the adds actually hitting it get killed. Bounded
                    // cadence (every 3 ticks) so we don't thrash single-target DPS.
                    if (cc.aoeCount >= effectiveAoeThreshold &&
                        (cc.currentTargetEntry == 0 || cc.currentTargetDistance > 12.f) &&
                        (++rec.addSwitchTicks % 3 == 0))
                    {
                        BotPosition ap;
                        uint64_t ag = 0;
                        if (_bridge->FindNearestHostile(rec.guid, 30.f, ap, ag) && ag != 0)
                            _bridge->SwitchTarget(rec.guid, ag);
                    }
                    // Frozen-AI breaker (see the reactive defend path): detect a stalled
                    // rotation by HP STAGNATION (hp flat while engaged). Objective progress
                    // already reset stepElapsedMs above and is the real success signal; here
                    // we only kick the rotation if hp is flat (no hits landing either way),
                    // re-asserting the attack on the nearest hostile every few ticks. No
                    // escape on kill steps — completing the kill is the goal; the step
                    // watchdog handles a genuinely impossible objective.
                    bool const hpMoved = rec.lastCombatHpPct < 0.f ||
                                         std::fabs(cc.hpPct - rec.lastCombatHpPct) > 2.0f;
                    rec.lastCombatHpPct = cc.hpPct;
                    if (hpMoved)
                        rec.combatStallTicks = 0;
                    else if (++rec.combatStallTicks % 4 == 0)
                    {
                        BotPosition hp;
                        uint64_t hg = 0;
                        if (_bridge->FindNearestHostile(rec.guid, 40.f, hp, hg) && hg != 0)
                            _bridge->AttackCreature(rec.guid, hg);
                    }
                }
                else if (rec.lootGraceTicks > 0)
                {
                    // LOOT — after combat, switch to non-combat engine (first tick) so
                    // LootNonCombatStrategy fires: it picks nearby lootable corpses from
                    // "available loot" (already populated by AttackCreature), moves to
                    // them, opens the loot window, and stores items including QuestRequired
                    // ones. Without this the bot stays in combat engine and never loots.
                    mode = "loot";
                    if (rec.lootGraceTicks == 18)
                    {
                        // Seed lastEngagedGuid into available-loot immediately so the
                        // bot loots its own kill before the "often"-timer scan fires —
                        // critical when other bots (random or idlebot) compete for mobs.
                        _bridge->BeginLoot(rec.guid, rec.lastEngagedGuid);
                        // Record kill classification on the first loot tick (combat just ended).
                        if (rec.lastEngagedGuid != 0)
                        {
                            TargetRole const role = IdleBotPullManager::Classify(
                                rec.lastEngagedGuid,
                                rec.lastEngagedWasObjective ? rec.lastEngagedGuid : 0,
                                rec.lastEngagedWasPathBlocker,
                                !rec.lastEngagedWasObjective && !rec.lastEngagedWasPathBlocker);
                            switch (role)
                            {
                                case TargetRole::ObjectiveTarget: ++rec.killStats.objectiveTarget; break;
                                case TargetRole::PathBlocker:     ++rec.killStats.pathBlocker;     break;
                                case TargetRole::DefensiveAdd:    ++rec.killStats.defensiveAdd;    break;
                                default:                          ++rec.killStats.accidental;      break;
                            }
                            LOG_INFO("module.idlebot",
                                "[IdleBot] {} kill role={} totals: obj={} blocker={} add={} acc={}",
                                rec.name, ToString(role),
                                rec.killStats.objectiveTarget, rec.killStats.pathBlocker,
                                rec.killStats.defensiveAdd, rec.killStats.accidental);
                            rec.lastEngagedGuid = 0;
                            rec.lastEngagedWasObjective = false;
                            rec.lastEngagedWasPathBlocker = false;
                        }
                    }
                    --rec.lootGraceTicks;
                    rec.stuckTicks = 0;
                }
                else if (!cc.valid || cc.possibleTargets == 0)
                {
                    // ROAM — no ATTACKABLE target here (the rest are tapped by other bots,
                    // or dead). Settle briefly, then wander within the area to find fresh
                    // mobs. "possible targets" already excludes tapped mobs, so 0 means
                    // genuinely nothing to fight (fixes the old tapped-mob freeze).
                    mode = "roam";
                    if (++rec.stuckTicks > 3)
                    {
                        rec.stuckTicks = 0;
                        RoamKillObjective(rec, step);
                    }
                }
                else if (cc.valid && rec.restTicks < _restMaxTicks &&
                         (cc.hpPct < static_cast<float>(effectiveRestHpPct) ||
                          (!rec.classConfig.skipManaRecover &&
                           cc.manaPct < static_cast<float>(effectiveRestManaPct))))
                {
                    // REST — top off before pulling the NEXT mob. Chain-pulling at half
                    // health is the #1 low-level death cause; a real player sits to
                    // eat/drink between pulls. Safe here: not engaged, no attacker, but
                    // attackable mobs remain (else ROAM caught it). Bounded by
                    // _restMaxTicks so a bot with no food / slow regen eventually pulls.
                    // Per-class thresholds: mage/priest rest to 70% mana; warrior skips mana gate.
                    mode = "rest";
                    ++rec.restTicks;
                    _bridge->Recover(rec.guid);
                    rec.stuckTicks = 0;
                }
                else
                {
                    // ENGAGE — untapped targets in sight. Stay near the objective area and
                    // prefer the guide's configured creature list when present. Pull cap:
                    // don't add targets past MaxPull (also implicit — we don't engage while
                    // already in combat).
                    mode = "engage";

                    // Danger gate: assess risk before committing to a pull. If a large
                    // pack is clustered here and we haven't started fighting yet, don't
                    // charge in — roam to find a safer entry point.
                    {
                        IdleBotDangerEvaluator::Config dcfg;
                        dcfg.maxPull       = effectiveMaxPull;
                        dcfg.criticalHpPct = _criticalHpPct;
                        dcfg.lowHpPct      = _lowHpPct;
                        dcfg.lowManaPct    = rec.classConfig.skipManaRecover ? 0 : _lowManaPct;
                        dcfg.packAvoidSize = _packAvoidSize;
                        DangerAssessment da = _dangerEval.Evaluate(cc, dcfg, rec.paused);
                        if (da.level == DangerLevel::Pause || da.level == DangerLevel::Avoid)
                        {
                            LOG_INFO("module.idlebot",
                                "[IdleBot] {} ENGAGE blocked by danger={} reason={}",
                                rec.name, ToString(da.level), da.reason);
                            RoamKillObjective(rec, step);
                            break;
                        }
                        if (da.level == DangerLevel::ClearFirst && cc.aoeCount >= _packAvoidSize)
                        {
                            // Moderate pack: pull from the edge rather than charging to center.
                            // Allow shouldAttack=true but don't override target selection —
                            // FindNearestHostile will naturally pick the closest edge mob.
                            // NOTE: mob_flood (panicFleeSize) is intentionally NOT checked here —
                            // aoeCount counts the densest cluster in view range, which is always
                            // high in typical starting zones. Panic-flee only fires in fight-mode
                            // when already in combat (see overwhelmed check).
                            LOG_DEBUG("module.idlebot",
                                "[IdleBot] {} ENGAGE large_pack aoe={} clearing_edge",
                                rec.name, cc.aoeCount);
                        }
                    }

                    bool shouldAttack = true;
                    BotPosition pos = _bridge->GetPosition(rec.guid);
                    BotPosition stepCenter;
                    stepCenter.mapId = step.coords.mapId;
                    stepCenter.x = step.coords.x;
                    stepCenter.y = step.coords.y;
                    stepCenter.z = step.coords.z;
                    stepCenter.valid = true;
                    BotPosition targetPos;
                    uint64_t questTargetGuid = 0;
                    float objectiveSearchRadius = step.coords.radius;
                    if (!step.creatureIds.empty() && objectiveSearchRadius < 120.f)
                        objectiveSearchRadius = 120.f;
                    if (objectiveSearchRadius > 220.f)
                        objectiveSearchRadius = 220.f;
                    bool const haveQuestTarget = _bridge->FindNearestQuestCreature(
                        rec.guid, step.creatureIds, stepCenter, objectiveSearchRadius, targetPos, questTargetGuid);

                    // Default: swing at the configured quest creature. We may instead
                    // pick a blocking add below so the bot fights a path to it.
                    uint64_t engageGuid = questTargetGuid;
                    BotPosition engagePos = targetPos;
                    float const PullRange = _pullDistance;

                    if (pos.valid && pos.mapId == step.coords.mapId)
                    {
                        float const dx = pos.x - step.coords.x;
                        float const dy = pos.y - step.coords.y;
                        if ((dx * dx + dy * dy) > step.coords.radius * step.coords.radius)
                        {
                            // Outside the objective area — close in before fighting.
                            _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, step.coords.radius);
                            shouldAttack = false;
                        }
                        else
                        {
                            // Inside the area. Take the quest creature if it is in pull
                            // range; otherwise clear the nearest add blocking the
                            // approach (player-like: don't run past hostiles to tunnel a
                            // far/protected captain — fight through the trash).
                            bool questInPull = false;
                            if (haveQuestTarget && targetPos.valid && targetPos.mapId == pos.mapId)
                            {
                                float const tx = pos.x - targetPos.x;
                                float const ty = pos.y - targetPos.y;
                                questInPull = (tx * tx + ty * ty) <= (PullRange * PullRange);
                            }

                            if (!questInPull && haveQuestTarget)
                            {
                                if (step.creatureIds.empty())
                                {
                                    // Unguided: clear any blocking add in the approach path
                                    BotPosition addPos;
                                    uint64_t addGuid = 0;
                                    if (_bridge->FindNearestHostile(rec.guid, step.coords.radius, addPos, addGuid) &&
                                        addGuid != 0 && addPos.valid && addPos.mapId == pos.mapId)
                                    {
                                        float const ax = pos.x - addPos.x;
                                        float const ay = pos.y - addPos.y;
                                        if ((ax * ax + ay * ay) <= (PullRange * PullRange))
                                        {
                                            engageGuid = addGuid;
                                            engagePos = addPos;
                                        }
                                    }
                                }
                                else
                                {
                                    // Guided step: quest target is out of pull range. Check whether
                                    // a hostile is physically between bot and target (path blocker)
                                    // before walking straight at the objective. Without this, guided
                                    // steps walk into camps and pull 3-5 mobs simultaneously.
                                    BotPosition addPos;
                                    uint64_t addGuid = 0;
                                    bool clearedBlocker = false;
                                    if (pos.valid && targetPos.valid &&
                                        _bridge->FindNearestHostile(rec.guid, PullRange, addPos, addGuid) &&
                                        addGuid != 0 && addPos.valid && addPos.mapId == pos.mapId)
                                    {
                                        BotPosition objPos;
                                        objPos.x = targetPos.x; objPos.y = targetPos.y;
                                        objPos.z = targetPos.z; objPos.valid = true;
                                        if (IdleBotPullManager::IsPathBlocker(pos, addPos, objPos, 12.f))
                                        {
                                            uint32_t const addLevel = _bridge->GetCreatureLevel(rec.guid, addGuid);
                                            uint32_t const botLevel = _bridge->GetLevel(rec.guid);
                                            if (IdleBotPullManager::CanPull(cc, addLevel, botLevel,
                                                addGuid, addPos, rec, effectiveMaxPull))
                                            {
                                                engageGuid = addGuid;
                                                engagePos = addPos;
                                                clearedBlocker = true;
                                                rec.lastEngagedWasPathBlocker = true;
                                                LOG_INFO("module.idlebot",
                                                    "[IdleBot] {} ENGAGE guided path_blocker cleared guid={} level={}",
                                                    rec.name, addGuid, addLevel);
                                            }
                                        }
                                    }
                                    if (!clearedBlocker)
                                    {
                                        _bridge->MoveTo(rec.guid, targetPos.mapId, targetPos.x, targetPos.y, targetPos.z, 5.f);
                                        shouldAttack = false;
                                    }
                                }
                            }

                            // Chosen target still out of pull range → move toward it.
                            if (engageGuid != 0 && engagePos.valid && engagePos.mapId == pos.mapId)
                            {
                                float const ex = pos.x - engagePos.x;
                                float const ey = pos.y - engagePos.y;
                                if ((ex * ex + ey * ey) > (PullRange * PullRange))
                                {
                                    _bridge->MoveTo(rec.guid, engagePos.mapId, engagePos.x, engagePos.y, engagePos.z, 5.f);
                                    shouldAttack = false;
                                }
                            }
                        }
                    }

                    if (!step.creatureIds.empty() && !haveQuestTarget && engageGuid == 0)
                    {
                        // No configured quest creature and nothing to clear → roam.
                        shouldAttack = false;
                        if (++rec.stuckTicks > 3)
                        {
                            rec.stuckTicks = 0;
                            RoamKillObjective(rec, step);
                        }
                    }

                    if (shouldAttack && engageGuid != 0 && cc.myAttackers < effectiveMaxPull)
                    {
                        // Skip mobs tagged by a real player (not bots).
                        // Disabled: on a private server with only bots, this blocks
                        // everything. The possibleTargets count already excludes tapped.
                        // if (_bridge->IsCreatureTappedByOther(rec.guid, engageGuid))
                        // {
                        //     shouldAttack = false;
                        //     RoamKillObjective(rec, step);
                        // }

                        // Target blacklist: skip mobs we couldn't reach.
                        auto bit = rec.targetBlacklist.find(engageGuid);
                        if (bit != rec.targetBlacklist.end() && rec.globalTick < bit->second)
                        {
                            shouldAttack = false;
                            RoamKillObjective(rec, step);
                        }

                        // Blackspot avoidance: skip mobs near stuck positions.
                        if (shouldAttack && engagePos.valid && !rec.blackspots.empty())
                        {
                            for (auto const& bs : rec.blackspots)
                            {
                                float bx = engagePos.x - bs.first, by = engagePos.y - bs.second;
                                if ((bx * bx + by * by) < 10.f * 10.f)
                                {
                                    shouldAttack = false;
                                    RoamKillObjective(rec, step);
                                    break;
                                }
                            }
                        }

                        // Level filter: skip mobs >5 levels above bot.
                        if (shouldAttack)
                        {
                            uint32_t mobLevel = _bridge->GetCreatureLevel(rec.guid, engageGuid);
                            uint32_t botLevel = _bridge->GetLevel(rec.guid);
                            if (mobLevel > 0 && mobLevel > botLevel + 5)
                            {
                                shouldAttack = false;
                                RoamKillObjective(rec, step);
                            }
                        }

                        // Track reach timeout for current target.
                        if (shouldAttack)
                        {
                            if (engageGuid != rec.targetReachGuid)
                            {
                                rec.targetReachGuid = engageGuid;
                                rec.targetReachTicks = 0;
                            }
                            if (++rec.targetReachTicks > 120)
                            {
                                rec.targetBlacklist[engageGuid] = rec.globalTick + 120;
                                rec.targetReachTicks = 0;
                                rec.targetReachGuid = 0;
                                LOG_INFO("module.idlebot",
                                    "[IdleBot] bot '{}': blacklisted unreachable target for 10min.",
                                    rec.name);
                                shouldAttack = false;
                                RoamKillObjective(rec, step);
                            }
                        }

                        if (shouldAttack)
                        {
                            if (_bridge->IsMounted(rec.guid))
                                _bridge->Dismount(rec.guid);
                            bool const attacked = _bridge->AttackCreature(rec.guid, engageGuid);
                            if (attacked)
                            {
                                rec.stuckTicks = 0;
                                rec.targetReachTicks = 0;
                                // Record what we're fighting so the LOOT transition
                                // can classify the kill.
                                rec.lastEngagedGuid = engageGuid;
                                rec.lastEngagedWasObjective = (engageGuid == questTargetGuid);
                                if (engageGuid == questTargetGuid)
                                    rec.lastEngagedWasPathBlocker = false;
                            }
                            else if (++rec.stuckTicks > 3)
                            {
                                rec.stuckTicks = 0;
                                RoamKillObjective(rec, step);
                            }
                        }
                    }
                }

                // Real-time diagnostics to the world log (readable live; gated by config).
                if (_debugEnabled && (rec.dbgThrottle++ % 3 == 0))
                {
                    BotPosition pos = _bridge->GetPosition(rec.guid);
                    InventoryStatus inv = _bridge->GetInventoryStatus(rec.guid);
                    LOG_INFO("module.idlebot",
                        "[IdleBot][dbg] {} q{} step{} mode={}: combat={} targets={} attackers={} aoe={} hp={:.0f}% mana={:.0f}% money={} free={}/{} grace={} progress={}/{} pos=({:.0f},{:.0f}) target={}:{}@{:.1f}",
                        rec.name, *step.questId, rec.currentStepIndex, mode, cc.inCombat ? 1 : 0,
                        cc.possibleTargets, cc.myAttackers, cc.aoeCount, cc.hpPct, cc.manaPct,
                        _bridge->GetMoney(rec.guid), inv.freeSlots, inv.totalSlots, rec.lootGraceTicks,
                        objectiveCurrent, objectiveRequired,
                        pos.valid ? pos.x : 0.f, pos.valid ? pos.y : 0.f,
                        cc.currentTargetEntry, cc.currentTargetName, cc.currentTargetDistance);
                }
            }
            break;
        }

        case StepType::InteractGameobject:
        {
            if (step.questId.has_value() &&
                RewindToQuestAcceptStep(rec, guide, *step.questId, "quest missing at object"))
                return;

            stepDone = HandleInteractGameObjectStep(rec, guide, step);
            break;
        }

        case StepType::CollectItems:
        {
            if (step.questId.has_value() &&
                RewindToQuestAcceptStep(rec, guide, *step.questId, "quest missing at collect"))
                return;

            stepDone = false;
            HandleCollectItemsStep(rec, guide, step, stepDone);
            break;
        }

        case StepType::UseItemOnNpc:
        {
            if (step.questId.has_value() &&
                RewindToQuestAcceptStep(rec, guide, *step.questId, "quest missing at use-item"))
                return;

            stepDone = HandleUseItemOnNpcStep(rec, guide, step);
            break;
        }

        case StepType::EscortQuest:
        {
            // Follow the quest NPC and defend it. The quest itself tracks completion.
            if (!step.questId.has_value())
            {
                stepDone = true;
                break;
            }
            QuestState qs = _bridge->GetQuestStatus(rec.guid, *step.questId);
            if (qs == QuestState::Complete || qs == QuestState::Rewarded)
            {
                stepDone = true;
                break;
            }
            if (qs == QuestState::NotStarted || qs == QuestState::Unknown)
            {
                stepDone = true;
                break;
            }
            // #47: tuned escort follow — 5yd trigger, 20yd max range, 300s timeout.
            if (rec.stepElapsedMs > 300000)
            {
                stepDone = true;
                break;
            }
            if (step.npcId.has_value())
            {
                uint64_t npcGuid = _bridge->FindNearestCreatureEntry(rec.guid, *step.npcId, 80.f);
                if (npcGuid != 0)
                {
                    _bridge->FollowCreature(rec.guid, npcGuid, 5.f);

                    BotPosition hostilePos;
                    uint64_t hostileGuid = 0;
                    if (_bridge->FindNearestHostile(rec.guid, 15.f, hostilePos, hostileGuid) &&
                        hostileGuid != 0 && !_bridge->IsInCombat(rec.guid))
                    {
                        if (_bridge->IsMounted(rec.guid))
                            _bridge->Dismount(rec.guid);
                        _bridge->AttackCreature(rec.guid, hostileGuid);
                    }
                }
                else
                    _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, 15.f);
            }
            rec.stepElapsedMs += _tickMs;
            break;
        }

        case StepType::TaxiRide:
        {
            // Walk to destination — no teleport. TaxiTo still teleports server-side
            // for cross-zone flights; use MoveTo for same-map destinations.
            if (step.coords.x != 0.f || step.coords.y != 0.f)
            {
                BotPosition bp = _bridge->GetPosition(rec.guid);
                if (bp.valid && bp.mapId == step.coords.mapId)
                {
                    float dx = bp.x - step.coords.x, dy = bp.y - step.coords.y;
                    if ((dx*dx + dy*dy) < 30.f * 30.f)
                    {
                        stepDone = true;
                        break;
                    }
                    _bridge->MoveTo(rec.guid, step.coords.mapId,
                        step.coords.x, step.coords.y, step.coords.z, 15.f);
                }
                else if (step.taxiNodeId.has_value())
                    _bridge->TaxiTo(rec.guid, *step.taxiNodeId);
                else
                    stepDone = true;
            }
            else
                stepDone = true;
            break;
        }

        case StepType::GossipInteract:
        {
            if (!step.npcId.has_value())
            {
                stepDone = true;
                break;
            }
            if (_bridge->IsMounted(rec.guid))
                _bridge->Dismount(rec.guid);  // #44
            uint64_t npcGuid = _bridge->FindNearestCreatureEntry(rec.guid, *step.npcId, 30.f);
            if (npcGuid == 0)
            {
                _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, 5.f);
                break;
            }
            uint32_t option = step.gossipOption.value_or(0);
            _bridge->GossipSelect(rec.guid, npcGuid, option);
            stepDone = true;
            break;
        }

        case StepType::UseItemAtLocation:
        {
            if (!step.itemId.has_value())
            {
                stepDone = true;
                break;
            }

            uint32_t const useItemId = *step.itemId;

            // Check quest completion first (may already be done from a prior tick's use).
            if (step.questId.has_value())
            {
                QuestState qs = _bridge->GetQuestStatus(rec.guid, *step.questId);
                if (qs == QuestState::Complete || qs == QuestState::Rewarded)
                {
                    stepDone = true;
                    break;
                }
            }

            // If bot doesn't have the item, skip rather than loop forever.
            if (_bridge->GetItemCount(rec.guid, useItemId, false) == 0)
            {
                LOG_WARN("module.idlebot",
                    "[IdleBot] QB_USE bot='{}' quest={} item={} reason=item_missing — skipping.",
                    rec.name, step.questId.value_or(0), useItemId);
                stepDone = true;
                break;
            }

            if (MoveToStepPosition(rec, step, 5.0f))
                break;
            if (_bridge->IsMounted(rec.guid))
                _bridge->Dismount(rec.guid);

            // Rate-limit: don't use the item every tick — item cooldowns and cast times
            // mean once every few ticks is realistic. Skip without counting stuckTicks.
            if (rec.useItemCooldownTicks < 3)
            {
                ++rec.useItemCooldownTicks;
                break;
            }
            rec.useItemCooldownTicks = 0;

            if (!step.creatureIds.empty())
            {
                // Targeted use: find a nearby creature and use the item ON it.
                uint64_t targetGuid = 0;
                for (uint32_t entry : step.creatureIds)
                {
                    targetGuid = _bridge->FindNearestCreatureEntry(rec.guid, entry, step.coords.radius);
                    if (targetGuid)
                        break;
                }
                if (targetGuid)
                {
                    _bridge->UseItemOnTarget(rec.guid, useItemId, targetGuid);

                    // Check per-use progress: if objective advanced, reset the retry counter.
                    uint32_t cur = 0, req = 0;
                    if (step.questId.has_value())
                        CompletionConditionMet(rec, step, &cur, &req);
                    if (cur > rec.useItemProgressCount)
                    {
                        rec.useItemProgressCount = cur;
                        rec.stuckTicks = 0;
                        LOG_INFO("module.idlebot",
                            "[IdleBot] QB_USE_TARGET bot='{}' quest={} item={} progress {}/{} — objective advancing.",
                            rec.name, step.questId.value_or(0), useItemId, cur, req);
                    }
                    else
                    {
                        LOG_INFO("module.idlebot",
                            "[IdleBot] QB_USE_TARGET bot='{}' quest={} item={} on={} (attempt {} progress={}/{})",
                            rec.name, step.questId.value_or(0), useItemId, targetGuid,
                            rec.stuckTicks + 1, cur, req);
                    }
                }
                else
                {
                    LOG_DEBUG("module.idlebot",
                        "[IdleBot] QB_USE_TARGET bot='{}' quest={} item={} no target found yet",
                        rec.name, step.questId.value_or(0), useItemId);
                }
            }
            else
            {
                _bridge->UseItem(rec.guid, useItemId);
                LOG_INFO("module.idlebot",
                    "[IdleBot] QB_USE bot='{}' quest={} item={} used (attempt {})",
                    rec.name, step.questId.value_or(0), useItemId, rec.stuckTicks + 1);
            }

            if (!step.questId.has_value())
            {
                stepDone = true;
                break;
            }

            // Timeout: skip if no progress after sufficient attempts. Count attempts,
            // not ticks, so the rate-limit above doesn't accelerate the skip timer.
            if (++rec.stuckTicks > 20)
            {
                LOG_WARN("module.idlebot",
                    "[IdleBot] QB_USE bot='{}' quest={} item={} — not completing after {} attempts (progress {}/? last), skipping.",
                    rec.name, *step.questId, useItemId, rec.stuckTicks, rec.useItemProgressCount);
                rec.stuckTicks = 0;
                rec.useItemProgressCount = 0;
                stepDone = true;
            }
            break;
        }

        case StepType::Vendor:
        {
            // Buy a specific item from a named vendor NPC.
            // Required guide fields: npc_id, item_id; optional: item_count (default 1).
            if (!step.npcId.has_value() || !step.itemId.has_value())
            {
                stepDone = true;
                break;
            }

            uint32_t const buyItemId = *step.itemId;
            uint32_t const buyCount  = step.itemCount > 0 ? step.itemCount : 1;

            // Already have the item → done.
            uint32_t const have = _bridge->GetItemCount(rec.guid, buyItemId, false);
            if (have >= buyCount)
            {
                LOG_INFO("module.idlebot",
                    "[IdleBot] QB_BUY bot='{}' item={} have={}/{} — done.",
                    rec.name, buyItemId, have, buyCount);
                stepDone = true;
                break;
            }

            // Move to the vendor's coordinates.
            if (MoveToStepPosition(rec, step, 8.0f))
                break;

            // Try to buy. The vendor must be within 10yd (enforced by BuyItem).
            bool const bought = _bridge->BuyItem(rec.guid, *step.npcId, buyItemId, static_cast<uint8_t>(buyCount - have));
            if (bought)
            {
                LOG_INFO("module.idlebot",
                    "[IdleBot] QB_BUY bot='{}' quest={} item={} bought.",
                    rec.name, step.questId.value_or(0), buyItemId);
                stepDone = true;
            }
            else
            {
                if (rec.stuckTicks == 0)
                    LOG_WARN("module.idlebot",
                        "[IdleBot] QB_BUY bot='{}' quest={} item={} — buy failed (NPC not close enough or item unavailable).",
                        rec.name, step.questId.value_or(0), buyItemId);
                if (++rec.stuckTicks > 20)
                {
                    LOG_WARN("module.idlebot",
                        "[IdleBot] QB_BUY bot='{}' item={} — buy failed after {} ticks, skipping.",
                        rec.name, buyItemId, rec.stuckTicks);
                    rec.stuckTicks = 0;
                    stepDone = true;
                }
            }
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

            // Category-aware IdleRPG feed: pick a verb/category from the step type
            // so the log reads like an RPG event stream rather than "step N done".
            char const* category = "GUIDE";
            switch (step.type)
            {
            case StepType::MoveTo:              category = "TRAVEL"; break;
            case StepType::AcceptQuest:         category = "QUEST";  break;
            case StepType::TurnInQuest:         category = "QUEST";  break;
            case StepType::KillMobs:            category = "COMBAT"; break;
            case StepType::InteractGameobject:  category = "QUEST";  break;
            case StepType::CollectItems:        category = "COLLECT"; break;
            case StepType::EscortQuest:         category = "QUEST";  break;
            case StepType::TaxiRide:            category = "TRAVEL"; break;
            case StepType::GossipInteract:      category = "QUEST";  break;
            case StepType::UseItemAtLocation:   category = "QUEST";  break;
            case StepType::Vendor:              category = "VENDOR"; break;
            default:                            category = "GUIDE";  break;
            }
            EmitEvent(rec, category, Acore::StringFormat("{} (step {}/{})",
                step.name, rec.currentStepIndex + 1, guide.steps.size()));
            AdvanceStep(rec);
        }
    }

    // -------------------------------------------------------------------------
    // Death handling (Priority 2). The playerbots DeadStrategy performs the actual
    // release/corpse-run/revive; idlebot observes + counts + persists, lets it run,
    // and only nudges/falls back when recovery stalls. Returns true while the bot
    // is dead or recovering so the caller skips the guide tick.
    // -------------------------------------------------------------------------
    bool IdleBotManager::HandleDeath(BotRecord& rec)
    {
        if (!_deathEnabled)
            return false;

        bool const dead = _bridge->IsDead(rec.guid);
        bool const ghost = _bridge->IsGhost(rec.guid);

        if (!dead && !ghost)
        {
            // Alive. If we were recovering, announce and reset the recovery state.
            if (rec.deathPhase != DeathPhase::Alive)
            {
                EmitEvent(rec, "RECOVERY", "back on its feet — resuming the guide");
                rec.deathPhase = DeathPhase::Alive;
                rec.corpseRunAttempts = 0;
                rec.ghostTicks = 0;
            }
            return false;
        }

        // First detection of this death.
        if (rec.deathPhase == DeathPhase::Alive)
        {
            rec.deathPhase = ghost ? DeathPhase::Ghost : DeathPhase::Dying;
            ++rec.deathCountTotal;
            ++rec.deathCountStep;
            rec.corpseRunAttempts = 0;
            rec.ghostTicks = 0;

            // Rate circuit breaker: 5 deaths in <600 ticks (~10 min) → pause for review.
            // Uses a 5-slot circular buffer of globalTick timestamps.
            {
                uint8_t const head = rec.deathRateHead % 5;
                uint32_t const oldest = rec.deathRateTicks[head];
                rec.deathRateTicks[head] = rec.globalTick;
                ++rec.deathRateHead;
                if (rec.deathRateHead >= 5 && rec.globalTick - oldest < 600)
                {
                    BlockBot(rec, "DEATH_LOOP", Acore::StringFormat(
                        "5 deaths in <10 min — rate circuit breaker triggered; pausing. "
                        "Use '.idlebot resume {}' to continue.", rec.name), true);
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': RATE CIRCUIT BREAKER — 5 deaths in <10 min. "
                        "Bot paused. Use '.idlebot resume {}' to continue.",
                        rec.name, rec.name);
                }
            }

            BotPosition pos = _bridge->GetPosition(rec.guid);
            if (pos.valid)
            {
                rec.lastDeathMap = pos.mapId;
                rec.lastDeathX = pos.x;
                rec.lastDeathY = pos.y;
                rec.lastDeathZ = pos.z;
                // #24: blackspot the death location after 2+ deaths on same step.
                if (rec.deathCountStep >= 2 && rec.blackspots.size() < 5)
                    rec.blackspots.emplace_back(pos.x, pos.y);
            }

            EmitEvent(rec, "DEATH", Acore::StringFormat("died (#{} total, #{} on this step)",
                rec.deathCountTotal, rec.deathCountStep));
            PersistProgress(rec);

            // Death loop on this step → pause for manual review. STRICT MODE ONLY:
            // organic bots have no steps (deathCountStep never resets via
            // AdvanceStep), and dying is a normal part of a long autonomous run —
            // pausing would silently abandon the bot forever. Let playerbots keep
            // reviving and questing instead.
            if (_pauseAfterDeathLoop && rec.decisionMode != "organic" &&
                rec.deathCountStep >= _maxDeathsPerStep)
            {
                // Don't dead-stop ("blocked") — that abandons the bot forever.
                if (SkipAllowedAtLevel(rec))
                {
                    // Combat too hard — quarantine, never skip.
                    // Death loops are a bug (combat/pull/guide) to fix, not a reason
                    // to advance progression. Other bots keep running.
                    BlockBot(rec, "COMBAT_TOO_HARD", Acore::StringFormat(
                        "[FAILURE:COMBAT_TOO_HARD] died {} times on step {} — "
                        "quarantining bot. Fix combat/pull/guide and use "
                        "'.idlebot resume {}' to continue.",
                        rec.deathCountStep, rec.currentStepIndex + 1, rec.name));
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': QUARANTINED — COMBAT_TOO_HARD on step {}. "
                        "Died {} times. Fix and resume.",
                        rec.name, rec.currentStepIndex + 1, rec.deathCountStep);
                }
                else
                {
                    ++rec.rescueRelocateCount;
                    if (_maxRescueRelocates > 0 && rec.rescueRelocateCount >= _maxRescueRelocates)
                    {
                        // Exhausted all rescue relocates. Walking back to the step
                        // anchor repeatedly has not resolved the combat failure. This
                        // is a combat/pathing/guide bug — NOT a valid skip reason.
                        // Quarantine (pause) so the bot stops corrupting progression
                        // data. Other bots keep running. Needs manual review + fix.
                        BlockBot(rec, "COMBAT_TOO_HARD", Acore::StringFormat(
                            "[FAILURE:COMBAT_TOO_HARD] walked back to quest area {}/{} times on step {} "
                            "and still dying — quarantining bot. Fix combat/pull/guide to unblock. "
                            "Use '.idlebot resume {}' after fixing.",
                            rec.rescueRelocateCount, _maxRescueRelocates,
                            rec.currentStepIndex + 1, rec.name));
                        LOG_WARN("module.idlebot",
                            "[IdleBot] bot '{}': QUARANTINED — COMBAT_TOO_HARD on step {}. "
                            "Walked back {}/{} times, still dying. Fix and resume.",
                            rec.name, rec.currentStepIndex + 1,
                            rec.rescueRelocateCount, _maxRescueRelocates);
                    }
                    else
                    {
                        if (_rescueRelocateBelowLevel)
                            rec.rescueRelocateRequested = true;
                        rec.deathCountStep = 0;
                        EmitEvent(rec, "RECOVERY", Acore::StringFormat(
                            "died {} times on step {} — too low (lvl<{}) to skip; "
                            "walking back to quest anchor and retrying ({}/{})",
                            _maxDeathsPerStep, rec.currentStepIndex + 1, _noSkipBelowLevel,
                            rec.rescueRelocateCount, _maxRescueRelocates));
                    }
                }
            }
            return true;
        }

        // Dead but not yet a ghost: let playerbots "auto release" handle it.
        // #23: don't release in dungeons — wait for a rez from party.
        if (!ghost && _bridge->IsDungeon(rec.guid))
            return true;
        // #16: soulstone — wait 8 ticks before releasing to give it time to proc.
        if (!ghost && _bridge->HasSoulstone(rec.guid) && rec.ghostTicks < 8)
        {
            ++rec.ghostTicks;
            return true;
        }
        if (!ghost)
        {
            rec.deathPhase = DeathPhase::Dying;
            return true;
        }

        // Ghost: corpse run in progress. Give playerbots a head start before nudging.
        rec.deathPhase = DeathPhase::Ghost;
        ++rec.ghostTicks;
        if (rec.ghostTicks < _ghostStallTicks)
            return true;

        // Stalled — scan for a safe spot and revive there.
        if (rec.corpseRunAttempts < _maxCorpseRunAttempts)
        {
            ++rec.corpseRunAttempts;
            rec.ghostTicks = 0;

            // Try to find a spot away from hostiles before reviving.
            float safeX, safeY, safeZ;
            if (_bridge->ScanSafeReviveSpot(rec.guid, 40.f, safeX, safeY, safeZ))
            {
                BotPosition pos = _bridge->GetPosition(rec.guid);
                if (pos.valid)
                    _bridge->MoveTo(rec.guid, pos.mapId, safeX, safeY, safeZ, 3.f);
            }

            _bridge->ReviveOrCorpseRun(rec.guid);
            EmitEvent(rec, "RECOVERY", Acore::StringFormat("corpse-run nudge {}/{}",
                rec.corpseRunAttempts, _maxCorpseRunAttempts));
            return true;
        }

        // Corpse run exhausted → graveyard revive, then direct-resurrect fallback.
        if (_allowGraveyardResurrect && _bridge->RequestSpiritHealerRevive(rec.guid))
        {
            EmitEvent(rec, "RECOVERY", "abandoning the corpse run — heading to the spirit healer");
            return true;
        }
        if (_allowDirectResurrect && _bridge->DirectResurrect(rec.guid))
        {
            EmitEvent(rec, "RECOVERY", "direct-resurrected (private-server fallback)");
            return true;
        }

        // Ghost stall: if spirit healer and direct-resurrect both failed for an
        // extended period, the corpse/spirit-healer path is broken. Quarantine the
        // bot and emit a structured failure so the root cause can be investigated.
        // Do NOT teleport — that hides the real navigation/interaction failure.
        if (rec.ghostTicks > 300 && !rec.paused)
        {
            BotPosition pos = _bridge->GetPosition(rec.guid);
            BlockBot(rec, "GHOST_RECOVERY_FAILED", Acore::StringFormat(
                "[FAILURE:GHOST_RECOVERY_FAILED] ghost state unresolved after >5 min "
                "(ghostTicks={}, corpseRunAttempts={}) — quarantining bot. "
                "Position: map={} ({:.0f},{:.0f},{:.0f}). "
                "Spirit healer path is broken or spirit healer interaction failed. "
                "Fix corpse-run/spirit-healer pathing, then use '.idlebot resume {}'.",
                rec.ghostTicks, rec.corpseRunAttempts,
                pos.valid ? pos.mapId : 0,
                pos.valid ? pos.x : 0.f,
                pos.valid ? pos.y : 0.f,
                pos.valid ? pos.z : 0.f,
                rec.name));
            LOG_WARN("module.idlebot",
                "[IdleBot] bot '{}': QUARANTINED — GHOST_RECOVERY_FAILED. "
                "Ghost for {}+ ticks with {} corpse-run attempts. "
                "Pos: map={} ({:.0f},{:.0f}). Fix spirit-healer path and resume.",
                rec.name, rec.ghostTicks, rec.corpseRunAttempts,
                pos.valid ? pos.mapId : 0,
                pos.valid ? pos.x : 0.f,
                pos.valid ? pos.y : 0.f);
        }

        // Nothing succeeded this tick; remain dead and retry next tick.
        return true;
    }

    // -------------------------------------------------------------------------
    // Inventory awareness / town maintenance (Priority 5). Returns true if a
    // maintenance action was performed this tick (consume the tick). When the bot
    // is not near the relevant NPC the playerbot actions fail harmlessly and we
    // return false so the bot keeps pursuing its objective (no infinite block).
    // -------------------------------------------------------------------------
    bool IdleBotManager::TickTransport(BotRecord& rec, GuideStep const& step)
    {
        using Phase = BotRecord::TransportPhase;

        BotPosition pos = _bridge->GetPosition(rec.guid);
        if (!pos.valid)
            return false;

        bool const onCorrectMap = (pos.mapId == step.coords.mapId);

        // Debug: log when transport triggers to catch map_id parsing issues.
        if (!onCorrectMap && rec.transportPhase == Phase::None)
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': step {} mapId={} but bot on mapId={} — transport triggered.",
                rec.name, rec.currentStepIndex, step.coords.mapId, pos.mapId);

        if (onCorrectMap && rec.transportPhase == Phase::None)
            return false;

        if (onCorrectMap && rec.transportPhase != Phase::None)
        {
            if (_bridge->IsOnTransport(rec.guid))
                _bridge->DisembarkTransport(rec.guid);
            rec.transportPhase = Phase::None;
            rec.transportTicks = 0;
            EmitEvent(rec, "TRAVEL", Acore::StringFormat(
                "arrived on map {} — resuming guide", pos.mapId));
            return false;
        }

        if (rec.transportPhase == Phase::None)
        {
            TransportRoute route;
            uint8_t team = _bridge->GetTeamId(rec.guid);

            // Teldrassil special case: bot is on Teldrassil island (map 1, either inside
            // the tree at z > 200, or at ground level in Rut'theran Village which is
            // separated from Auberdine by water at x > 8200, y > 700).
            // The only exit is the Darnassus portal (AreaTrigger 527) → Rut'theran
            // Village, then Moonspray (176244) to Auberdine.  After disembarking at
            // Auberdine the transport phase resets, and the normal Bravery route
            // (Auberdine → SW Harbor) fires on the next tick.
            bool const inTeldrassil = (pos.mapId == 1 && team == 0 &&
                (pos.z > 200.f || (pos.x > 8200.f && pos.y > 700.f)));
            if (inTeldrassil)
            {
                route = { 176244, 1, 8680.f, 960.f, 10.f,
                          1, 6443.f, 413.f, 9.f,
                          "Moonspray (Rut'theran→Auberdine)" };
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': in Teldrassil (z={:.0f}) — taking Moonspray first.",
                    rec.name, pos.z);
            }
            else if (!FindTransportRoute(pos.mapId, step.coords.mapId, team, route))
            {
                LOG_WARN("module.idlebot",
                    "[IdleBot] bot '{}': no transport route from map {} to map {}.",
                    rec.name, pos.mapId, step.coords.mapId);
                return false;
            }
            rec.transportEntry = route.transportEntry;
            rec.transportDestMap = route.destMapId;
            rec.transportDestX = route.destX;
            rec.transportDestY = route.destY;
            rec.transportDestZ = route.destZ;
            rec.transportPhase = Phase::TravelToDock;
            rec.transportTicks = 0;
            EmitEvent(rec, "TRAVEL", Acore::StringFormat(
                "need map {} — taking {} to get there", step.coords.mapId, route.name));
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': cross-continent travel via {} (entry {}).",
                rec.name, route.name, route.transportEntry);

            _bridge->MoveTo(rec.guid, route.dockMapId,
                route.dockX, route.dockY, route.dockZ, 15.f);
            return true;
        }

        ++rec.transportTicks;

        switch (rec.transportPhase)
        {
            case Phase::TravelToDock:
            {
                TransportRoute route;
                uint8_t team = _bridge->GetTeamId(rec.guid);
                if (!FindTransportRoute(pos.mapId, rec.transportDestMap, team, route))
                {
                    rec.transportPhase = Phase::None;
                    return false;
                }

                float ddx = pos.x - route.dockX, ddy = pos.y - route.dockY;
                if ((ddx * ddx + ddy * ddy) <= 30.f * 30.f)
                {
                    rec.transportPhase = Phase::WaitForTransport;
                    rec.transportTicks = 0;
                    rec.waypointChainIdx = 0;
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': at dock, waiting for transport entry {}.",
                        rec.name, rec.transportEntry);
                    return true;
                }

                WaypointChain const* chain = nullptr;
                uint32_t startIdx = 0;
                if (FindWaypointChain(pos.mapId, pos.x, pos.y,
                    route.dockMapId, route.dockX, route.dockY, team, chain, startIdx))
                {
                    // Start from the waypoint AFTER the nearest one so we always
                    // move forward through the chain, not back toward a passed point.
                    uint32_t idx = std::max(rec.waypointChainIdx, startIdx + 1);
                    // Skip waypoints on a different map (post-transition).
                    uint32_t preSkip = idx;
                    while (idx < chain->count && chain->points[idx].mapId != pos.mapId)
                        ++idx;
                    if (idx != preSkip)
                        LOG_INFO("module.idlebot",
                            "[IdleBot] bot '{}': skipped waypoints {}-{} (different map), now at {}.",
                            rec.name, preSkip + 1, idx, idx + 1);
                    rec.waypointChainIdx = idx;
                    if (idx < chain->count)
                    {
                        auto const& wp = chain->points[idx];
                        float wx = pos.x - wp.x, wy = pos.y - wp.y;
                        if ((wx * wx + wy * wy) < 30.f * 30.f)
                        {
                            if (wp.areaTrigger != 0)
                            {
                                _bridge->FireAreaTrigger(rec.guid, wp.areaTrigger);
                                LOG_INFO("module.idlebot",
                                    "[IdleBot] bot '{}': waypoint {}/{} — firing areatrigger {}.",
                                    rec.name, idx + 1, chain->count, wp.areaTrigger);
                            }
                            ++rec.waypointChainIdx;
                            if (rec.transportTicks % 30 == 0)
                                LOG_INFO("module.idlebot",
                                    "[IdleBot] bot '{}': waypoint {}/{} of '{}' reached.",
                                    rec.name, idx + 1, chain->count, chain->name);
                        }
                        else
                        {
                            _bridge->MoveTo(rec.guid, wp.mapId, wp.x, wp.y, wp.z, 10.f);
                            if (rec.transportTicks % 10 == 0)
                                LOG_INFO("module.idlebot",
                                    "[IdleBot] bot '{}': walking to waypoint {}/{} '{}' ({:.0f},{:.0f} → {:.0f},{:.0f}).",
                                    rec.name, idx + 1, chain->count, chain->name,
                                    pos.x, pos.y, wp.x, wp.y);
                        }
                        return true;
                    }
                }

                _bridge->MoveTo(rec.guid, route.dockMapId,
                    route.dockX, route.dockY, route.dockZ, 15.f);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': walking toward dock ({:.0f},{:.0f} → {:.0f},{:.0f}).",
                    rec.name, pos.x, pos.y, route.dockX, route.dockY);
                // Throttle this log to every 30 ticks.
                // (the old log was misleadingly "at dock" when we're still en route)
                LOG_DEBUG("module.idlebot",
                    "[IdleBot] bot '{}': no waypoint chain matched — direct walk to dock.",
                    rec.name, rec.transportEntry);
                return true;
            }
            case Phase::WaitForTransport:
            {
                if (_bridge->IsTransportStopped(rec.guid, rec.transportEntry,
                    pos.x, pos.y, pos.z, 500.f))
                {
                    rec.transportPhase = Phase::Boarding;
                    rec.transportTicks = 0;
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': transport arrived — boarding.",
                        rec.name);
                }
                else if (rec.transportTicks % 30 == 0)
                {
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': waiting for transport ({}s)...",
                        rec.name, rec.transportTicks);
                    if (rec.transportTicks % 60 == 0)
                        _bridge->LogTransportPositions(rec.guid, rec.transportEntry);
                }
                // Safety valve: if the transport hasn't arrived in 720s (12 min),
                // reset the full transport phase so we re-approach the dock.
                // This guards against IsTransportStopped missing the transport or
                // the transport being delayed for any reason.
                if (rec.transportTicks >= 720)
                {
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': transport wait timeout ({}s) — retrying.",
                        rec.name, rec.transportTicks);
                    rec.transportPhase = Phase::None;
                    rec.transportTicks = 0;
                }
                return true;
            }
            case Phase::Boarding:
            {
                if (_bridge->BoardTransport(rec.guid, rec.transportEntry))
                {
                    rec.transportPhase = Phase::Riding;
                    rec.transportTicks = 0;
                    EmitEvent(rec, "TRAVEL", "boarded transport — crossing");
                }
                else if (rec.transportTicks > 10)
                {
                    rec.transportPhase = Phase::WaitForTransport;
                    rec.transportTicks = 0;
                }
                return true;
            }
            case Phase::Riding:
            {
                if (!_bridge->IsOnTransport(rec.guid))
                {
                    rec.transportPhase = Phase::None;
                    rec.transportTicks = 0;
                    return false;
                }
                // Same-map intermediate transport (e.g. Moonspray Rut'theran→Auberdine):
                // disembark when the transport arrives near the destination.
                if (rec.transportDestMap == pos.mapId)
                {
                    float ddx = pos.x - rec.transportDestX, ddy = pos.y - rec.transportDestY;
                    if ((ddx * ddx + ddy * ddy) < 200.f * 200.f)
                    {
                        LOG_INFO("module.idlebot",
                            "[IdleBot] bot '{}': intermediate transport reached dest ({:.0f},{:.0f}) — disembarking.",
                            rec.name, pos.x, pos.y);
                        EmitEvent(rec, "TRAVEL", "intermediate transport arrived — disembarking");
                        _bridge->DisembarkTransport(rec.guid);
                        rec.transportPhase = Phase::None;
                        rec.transportTicks = 0;
                        return false;
                    }
                }
                if (rec.transportTicks > 600)
                {
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}': transport ride timeout — disembarking and retrying.",
                        rec.name);
                    _bridge->DisembarkTransport(rec.guid);
                    rec.transportPhase = Phase::TravelToDock;
                    rec.transportTicks = 0;
                }
                return true;
            }
            case Phase::Disembarking:
            {
                _bridge->DisembarkTransport(rec.guid);
                rec.transportPhase = Phase::None;
                rec.transportTicks = 0;
                return false;
            }
            default:
                return false;
        }
    }

    // TickClassTravel
    // -------------------------------------------------------------------------
    // Handle steps whose target zone can only be reached via a class-specific
    // teleport spell, where normal pathfinding is insufficient:
    //
    //   Druid: Teleport: Moonglade — step in Moonglade zone (map 1), bot not
    //     in Moonglade. Uses TeleportBot (private-server direct) to land at the
    //     Nighthaven arrival point, then the normal TurnInQuest executor walks
    //     to the NPC.
    //
    //   Mage: teleport spells — stub; extend here when needed.
    //
    // Called from TickBot after TickTransport (same-map class travel) and before
    // the step executor. Returns true while transit is pending (consumes the tick).
    bool IdleBotManager::TickClassTravel(BotRecord& rec, GuideStep const& step)
    {
        if (!StepHasCoordinates(step))
            return false;

        BotPosition pos = _bridge->GetPosition(rec.guid);
        if (!pos.valid)
            return false;

        // ---- Druid: Teleport: Moonglade ----
        // Moonglade is on Kalimdor (map 1). Zone bounds (conservative):
        //   x: [7000, 9200]   y: [-4000, -2000]
        // A Night Elf druid starts in Teldrassil (map 1, z > 200 or x > 8200)
        // and cannot walk to Moonglade through the mountains. Use TeleportBot.
        // Nighthaven arrival point: x=7940, y=-2512, z=489 (near inn/quest hub).
        static constexpr uint8_t kClassDruid = 11;
        static constexpr float kMgX1 = 7000.f, kMgX2 = 9200.f;
        static constexpr float kMgY1 = -4000.f, kMgY2 = -2000.f;
        static constexpr float kMgArrX = 7940.f, kMgArrY = -2512.f, kMgArrZ = 489.f;

        auto inMoonglade = [](float x, float y, uint32_t mapId) -> bool {
            return mapId == 1
                && x >= kMgX1 && x <= kMgX2
                && y >= kMgY1 && y <= kMgY2;
        };

        bool const stepInMoonglade = inMoonglade(step.coords.x, step.coords.y, step.coords.mapId);
        if (stepInMoonglade)
        {
            bool const botInMoonglade = inMoonglade(pos.x, pos.y, pos.mapId);
            if (botInMoonglade)
            {
                // Already in Moonglade — reset counter and let the step executor run.
                if (rec.classTravelTicks > 0)
                {
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': arrived in Moonglade (step {}) — resuming guide.",
                        rec.name, rec.currentStepIndex + 1);
                    rec.classTravelTicks = 0;
                }
                return false;
            }

            uint8_t const botClass = _bridge->GetClass(rec.guid);
            if (botClass != kClassDruid)
            {
                // Non-druid hitting a Moonglade-targeted step — class_mask should
                // have filtered this. Log once and fall through (step will time out).
                if (rec.classTravelTicks == 0)
                    LOG_ERROR("module.idlebot",
                        "[IdleBot][FAILURE:SPECIAL_TRAVEL_UNSUPPORTED] bot '{}' step {} "
                        "targets Moonglade but class={} is not druid — "
                        "class_mask restriction should have prevented this.",
                        rec.name, rec.currentStepIndex + 1, botClass);
                rec.classTravelTicks = 1;   // mark as seen; don't return true (let it proceed)
                return false;
            }

            // Druid not in Moonglade — issue Teleport: Moonglade via TeleportBot.
            if (rec.classTravelTicks == 0)
            {
                EmitEvent(rec, "TRAVEL", Acore::StringFormat(
                    "druid class travel: teleporting to Moonglade for step {}",
                    rec.currentStepIndex + 1));
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': druid Teleport: Moonglade — step {} needs Moonglade, "
                    "currently map={} ({:.0f},{:.0f},{:.0f}).",
                    rec.name, rec.currentStepIndex + 1,
                    pos.mapId, pos.x, pos.y, pos.z);
            }

            // Attempt every 3 ticks; TeleportBot is a no-op while in combat or flight.
            // NOTE: TeleportBot is used here because playerbots has no separate API to cast
            // a class teleport spell. This is the ONLY approved TeleportBot call in runtime
            // questing code — it implements the "Teleport: Moonglade" class spell mechanic.
            // See docs/PLAYERLIKE_POLICY.md — Druid Teleport: Moonglade exception.
            if (rec.classTravelTicks % 3 == 0)
            {
                bool const inCombat = _bridge->IsInCombat(rec.guid);
                if (!inCombat)
                {
                    _bridge->TeleportBot(rec.guid, 1, kMgArrX, kMgArrY, kMgArrZ);
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': druid teleport to Moonglade issued (attempt {}).",
                        rec.name, rec.classTravelTicks / 3 + 1);
                }
                else if (rec.classTravelTicks % 15 == 0)
                {
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': druid Moonglade teleport deferred — in combat ({}s elapsed).",
                        rec.name, rec.classTravelTicks);
                }
            }

            ++rec.classTravelTicks;

            // Safety valve: after 120s of failed teleports (stuck in combat / flight),
            // log a hard failure and stop blocking — the step watchdog will eventually
            // skip so the guide isn't wedged indefinitely.
            if (rec.classTravelTicks > 120)
            {
                LOG_ERROR("module.idlebot",
                    "[IdleBot][FAILURE:SPECIAL_TRAVEL_FAILED] bot '{}' step {} — "
                    "Moonglade teleport failed after {}s. Bot cannot reach destination. "
                    "Will allow step watchdog to skip.",
                    rec.name, rec.currentStepIndex + 1, rec.classTravelTicks);
                rec.classTravelTicks = 0;
                return false;   // stop blocking; step watchdog handles the rest
            }

            return true;    // consume tick while in transit
        }

        // Add more class-travel zones here as needed.
        // Example pattern for mage portals:
        //   if (stepInSomeCity && botClass == kClassMage) { ... return true; }

        return false;
    }

    bool IdleBotManager::TickUnstick(BotRecord& rec)
    {
        BotPosition pos = _bridge->GetPosition(rec.guid);
        if (!pos.valid || _bridge->IsInCombat(rec.guid))
        {
            rec.posStallTicks = 0;
            return false;
        }

        // Suppress unstick while intentionally waiting for a GO respawn.
        if (rec.objectWaitMs > 0 && rec.lastObjectGuid == 0)
        {
            rec.posStallTicks = 0;
            return false;
        }

        // Suppress unstick when bot is within approach range of a TurnInQuest or
        // AcceptQuest step — the bot is deliberately stationary at the NPC, not stuck.
        // Without this suppression, TickUnstick fires every 15 ticks and prevents the
        // step executor (TurnInQuest/AcceptQuest) from ever running.
        {
            auto git = _guides.find(rec.guideId);
            if (git != _guides.end() && rec.currentStepIndex < git->second.steps.size())
            {
                GuideStep const& st = git->second.steps[rec.currentStepIndex];
                bool const isNpcStep = (st.type == StepType::TurnInQuest ||
                                        st.type == StepType::AcceptQuest);
                if (isNpcStep && StepHasCoordinates(st))
                {
                    float const ddx = pos.x - st.coords.x;
                    float const ddy = pos.y - st.coords.y;
                    float const ddz = pos.z - st.coords.z;
                    float const distSq3d = ddx * ddx + ddy * ddy + ddz * ddz;
                    constexpr float kNpcApproachRadius = 30.0f;
                    if (distSq3d <= kNpcApproachRadius * kNpcApproachRadius)
                    {
                        rec.posStallTicks = 0;
                        return false;
                    }
                }
            }
        }

        float dx = pos.x - rec.lastPosX;
        float dy = pos.y - rec.lastPosY;
        float distSq = dx * dx + dy * dy;

        // Moved significantly → reset stall counter and escalation.
        if (distSq > 10.f * 10.f)
        {
            rec.posStallTicks = 0;
            rec.unstickAttempt = 0;
        }
        // Barely moved (< 2 yards) → increment stall.
        else if (distSq < 2.f * 2.f)
            ++rec.posStallTicks;
        else
            rec.posStallTicks = 0;

        rec.lastPosX = pos.x;
        rec.lastPosY = pos.y;

        // Not stalled long enough to act (need ~5s of no movement).
        if (rec.posStallTicks < 15)
            return false;

        rec.posStallTicks = 0;

        // Gather step context once for all stuck messages.
        std::string stepCtx;
        {
            auto git = _guides.find(rec.guideId);
            if (git != _guides.end() && rec.currentStepIndex < git->second.steps.size())
            {
                auto const& s = git->second.steps[rec.currentStepIndex];
                stepCtx = Acore::StringFormat(" at step {}/{} '{}'",
                    rec.currentStepIndex + 1, git->second.steps.size(), s.name);
            }
        }

        switch (rec.unstickAttempt)
        {
            case 0:
                _bridge->JumpForward(rec.guid, 5.f);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}':{} stuck — trying jump forward.", rec.name, stepCtx);
                break;
            case 1:
                _bridge->StrafeMove(rec.guid, true, 8.f);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}':{} stuck — trying strafe left.", rec.name, stepCtx);
                break;
            case 2:
                _bridge->StrafeMove(rec.guid, false, 8.f);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}':{} stuck — trying strafe right.", rec.name, stepCtx);
                break;
            case 3:
                _bridge->MoveBackward(rec.guid, 10.f);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}':{} stuck — trying reverse.", rec.name, stepCtx);
                break;
            default:
            {
                // Add current position as a blackspot so we don't path here again.
                BotPosition bp = _bridge->GetPosition(rec.guid);
                if (bp.valid && rec.blackspots.size() < 5)
                {
                    rec.blackspots.push_back({bp.x, bp.y});
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': blackspotted ({:.0f},{:.0f}).",
                        rec.name, bp.x, bp.y);
                }

                // If we're stuck trying to reach a GO, blacklist it — unreachable
                // terrain (cliff, building interior) won't get better with more tries.
                if (rec.lastObjectGuid != 0)
                {
                    rec.objectLocalBlacklist[rec.lastObjectGuid] = rec.globalTick + 120;
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': stuck exhausted reaching go_guid={} — blacklisting for 120 ticks.",
                        rec.name, rec.lastObjectGuid);
                    rec.lastObjectGuid = 0;
                }

                // Walk toward the step anchor (no teleport).
                auto git = _guides.find(rec.guideId);
                if (git != _guides.end() && rec.currentStepIndex < git->second.steps.size())
                {
                    auto const& s = git->second.steps[rec.currentStepIndex];
                    _bridge->MoveTo(rec.guid, s.coords.mapId,
                        s.coords.x, s.coords.y, s.coords.z, 5.f);
                    LOG_WARN("module.idlebot",
                        "[IdleBot] bot '{}':{} stuck exhausted all unstick attempts — walking to step anchor.",
                        rec.name, stepCtx);
                    EmitEvent(rec, "RECOVERY", "stuck — walking to step anchor");
                }
                rec.unstickAttempt = 0;
                return true;
            }
        }
        ++rec.unstickAttempt;
        return true;
    }

    bool IdleBotManager::MaintenanceGuard(BotRecord& rec)
    {
        if (!_townMaintenanceEnabled)
            return false;

        InventoryStatus inv = _bridge->GetInventoryStatus(rec.guid);
        if (!inv.valid)
            return false;

        bool const bagsCritical = (inv.freeSlots <= 2);
        bool const repairLow =
            inv.needsRepair || inv.lowestDurabilityPct < _repairBelowDurabilityPct;

        // Already on a vendor run — keep walking toward the vendor.
        if (rec.maintaining)
        {
            ++rec.maintTicks;
            if (rec.maintTicks > 300)
            {
                LOG_WARN("module.idlebot",
                    "[IdleBot] bot '{}': vendor run timed out after 5 min.",
                    rec.name);
                rec.maintaining = false;
                rec.maintTicks = 0;
                return false;
            }

            // Try to sell/repair — sell gray+white+green with quest item protection.
            bool sold = false;
            if (bagsCritical)
            {
                uint32_t count = _bridge->SellByQuality(rec.guid, ITEM_QUALITY_UNCOMMON);
                if (count > 0)
                {
                    EmitEvent(rec, "VENDOR", Acore::StringFormat("sold {} items ({} free before)",
                        count, inv.freeSlots));
                    sold = true;
                }
            }
            if (repairLow)
                _bridge->Repair(rec.guid);

            InventoryStatus postInv = _bridge->GetInventoryStatus(rec.guid);
            if (postInv.valid && postInv.freeSlots > 5)
            {
                _bridge->BuyFood(rec.guid);
                // #9/#11: hunters buy ammo at vendor.
                if (_bridge->GetClass(rec.guid) == 3 /*CLASS_HUNTER*/)
                    _bridge->DoBotAction(rec.guid, "buy");
                // #68: skinning stub.
                if (_skinMobs)
                    _bridge->DoBotAction(rec.guid, "skin");
                rec.maintaining = false;
                rec.maintTicks = 0;
                EmitEvent(rec, "VENDOR", Acore::StringFormat(
                    "vendor run done — {} free slots now", postInv.freeSlots));
                return sold;
            }

            // Follow the vendor return route (guide steps backtracked to town).
            if (!rec.vendorRoute.empty() && rec.vendorRouteIdx < rec.vendorRoute.size())
            {
                auto const& wp = rec.vendorRoute[rec.vendorRouteIdx];
                BotPosition pos = _bridge->GetPosition(rec.guid);
                if (pos.valid)
                {
                    float dx = pos.x - wp.x, dy = pos.y - wp.y;
                    if ((dx * dx + dy * dy) < 25.f * 25.f)
                    {
                        ++rec.vendorRouteIdx;
                        LOG_INFO("module.idlebot",
                            "[IdleBot] bot '{}': vendor route waypoint {}/{} reached.",
                            rec.name, rec.vendorRouteIdx, rec.vendorRoute.size());
                    }
                    else
                    {
                        _bridge->MoveTo(rec.guid, wp.mapId, wp.x, wp.y, wp.z, 10.f);
                        if (rec.maintTicks % 15 == 0)
                            LOG_INFO("module.idlebot",
                                "[IdleBot] bot '{}': vendor run — walking to waypoint {}/{} ({:.0f},{:.0f} → {:.0f},{:.0f}).",
                                rec.name, rec.vendorRouteIdx + 1, rec.vendorRoute.size(),
                                pos.x, pos.y, wp.x, wp.y);
                    }
                }
                return true;
            }

            // Route exhausted or none — try nearby vendor or hub direct.
            BotPosition vendorPos;
            uint64_t vendorGuid = 0;
            if (_bridge->FindNearestServiceNpc(rec.guid, 0x80 /*UNIT_NPC_FLAG_VENDOR*/,
                200.f, vendorPos, vendorGuid) && vendorPos.valid)
                _bridge->MoveTo(rec.guid, vendorPos.mapId, vendorPos.x, vendorPos.y, vendorPos.z, 5.f);

            return true;
        }

        if (!bagsCritical && !repairLow)
            return false;

        // Try to sell/repair immediately (maybe already near a vendor).
        if (bagsCritical)
        {
            uint32_t count = _bridge->SellByQuality(rec.guid, ITEM_QUALITY_UNCOMMON);
            if (count > 0)
            {
                EmitEvent(rec, "VENDOR", Acore::StringFormat("sold {} items ({} free before)",
                    count, inv.freeSlots));
                return true;
            }
        }
        if (repairLow && _bridge->Repair(rec.guid))
        {
            EmitEvent(rec, "REPAIR", Acore::StringFormat("repaired (was {}% dur)",
                inv.lowestDurabilityPct));
            return true;
        }

        // 1) Check for a vendor within 200yd (nearby — quick walk).
        BotPosition vendorPos;
        uint64_t vendorGuid = 0;
        if (_bridge->FindNearestServiceNpc(rec.guid, 0x80 /*UNIT_NPC_FLAG_VENDOR*/,
            200.f, vendorPos, vendorGuid) && vendorPos.valid)
        {
            rec.maintaining = true;
            rec.maintTicks = 0;
            _bridge->MoveTo(rec.guid, vendorPos.mapId, vendorPos.x, vendorPos.y, vendorPos.z, 5.f);
            EmitEvent(rec, "TOWN", Acore::StringFormat(
                "bags full ({} free) — walking to nearby vendor", inv.freeSlots));
            return true;
        }

        // 2) Use embedded vendor coords from the guide (nearest_vendor field).
        {
            auto git = _guides.find(rec.guideId);
            BotPosition pos = _bridge->GetPosition(rec.guid);
            if (git != _guides.end() && pos.valid)
            {
                auto const& guide = git->second;
                for (int32_t i = static_cast<int32_t>(rec.currentStepIndex);
                     i >= 0 && i > static_cast<int32_t>(rec.currentStepIndex) - 30; --i)
                {
                    auto const& s = guide.steps[i];
                    if (!s.vendorEntry.has_value() || s.vendorCoords.mapId != pos.mapId)
                        continue;
                    rec.maintaining = true;
                    rec.maintTicks = 0;
                    rec.vendorRoute.clear();
                    rec.vendorRouteIdx = 0;
                    // Build a route: backtrack through steps to the vendor.
                    for (int32_t j = static_cast<int32_t>(rec.currentStepIndex) - 1; j >= i && rec.vendorRoute.size() < 15; --j)
                    {
                        auto const& ws = guide.steps[j];
                        if (ws.coords.mapId != pos.mapId || (ws.coords.x == 0.f && ws.coords.y == 0.f))
                            continue;
                        if (!rec.vendorRoute.empty())
                        {
                            auto const& last = rec.vendorRoute.back();
                            float dx = last.x - ws.coords.x, dy = last.y - ws.coords.y;
                            if ((dx * dx + dy * dy) < 80.f * 80.f)
                                continue;
                        }
                        rec.vendorRoute.push_back(ws.coords);
                    }
                    rec.vendorRoute.push_back(s.vendorCoords);
                    EmitEvent(rec, "TOWN", Acore::StringFormat(
                        "bags full — heading to known vendor via {} waypoints", rec.vendorRoute.size()));
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': vendor run to guide-embedded vendor (entry {}).",
                        rec.name, *s.vendorEntry);
                    return true;
                }
            }
        }

        // 3) Build a return route by backtracking through guide steps (follows roads).
        //    Scan backward from current step looking for a step near a vendor NPC or town.
        {
            auto git = _guides.find(rec.guideId);
            BotPosition pos = _bridge->GetPosition(rec.guid);
            if (git != _guides.end() && pos.valid)
            {
                auto const& guide = git->second;
                rec.vendorRoute.clear();
                rec.vendorRouteIdx = 0;

                // Collect unique coords walking backward through recent steps.
                for (int32_t i = static_cast<int32_t>(rec.currentStepIndex) - 1; i >= 0 && rec.vendorRoute.size() < 20; --i)
                {
                    auto const& s = guide.steps[i];
                    if (s.coords.mapId != pos.mapId || (s.coords.x == 0.f && s.coords.y == 0.f))
                        continue;
                    // Skip coords too close to the last one (dedup).
                    if (!rec.vendorRoute.empty())
                    {
                        auto const& last = rec.vendorRoute.back();
                        float dx = last.x - s.coords.x, dy = last.y - s.coords.y;
                        if ((dx * dx + dy * dy) < 80.f * 80.f)
                            continue;
                    }
                    rec.vendorRoute.push_back(s.coords);
                }

                // Append the zone hub as the final destination.
                uint8_t faction = _bridge->GetTeamId(rec.guid);
                uint8_t race = _bridge->GetRace(rec.guid);
                uint32_t level = _bridge->GetLevel(rec.guid);
                LevelHub hub;
                if (NextHubFor(faction, race, level, hub) && hub.mapId == pos.mapId)
                {
                    Coordinates hubCoord;
                    hubCoord.mapId = hub.mapId;
                    hubCoord.x = hub.x;
                    hubCoord.y = hub.y;
                    hubCoord.z = hub.z;
                    rec.vendorRoute.push_back(hubCoord);
                }

                if (!rec.vendorRoute.empty())
                {
                    rec.maintaining = true;
                    rec.maintTicks = 0;
                    EmitEvent(rec, "TOWN", Acore::StringFormat(
                        "bags full — backtracking {} waypoints to town via roads",
                        rec.vendorRoute.size()));
                    LOG_INFO("module.idlebot",
                        "[IdleBot] bot '{}': bags full — vendor run via {} road waypoints.",
                        rec.name, rec.vendorRoute.size());
                    return true;
                }
            }
        }

        // 3) Last resort: hearthstone.
        if (_bridge->HasHearthstone(rec.guid) && _bridge->IsHearthstoneReady(rec.guid))
        {
            _bridge->UseHearthstone(rec.guid);
            rec.maintaining = true;
            rec.maintTicks = 0;
            EmitEvent(rec, "TOWN", "no vendor or town nearby — hearthing to sell");
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': bags full, no town — using hearthstone.",
                rec.name);
            return true;
        }

        return false;
    }

    // One-time per-session strategy setup: ensure looting is on so kills and
    // gameobjects (Q3902) get picked up. Re-runs after a relogin.
    void IdleBotManager::EnsureStrategies(BotRecord& rec)
    {
        BotLiveStatus st;
        _bridge->GetLiveStatus(rec.guid, st);
        if (!st.online || !st.controlled)
        {
            rec.strategiesEnsured = false;
            return;
        }
        if (rec.strategiesEnsured)
            return;

        _bridge->SetNonCombatStrategy(rec.guid, "+loot");
        _bridge->DoBotAction(rec.guid, "buff");  // #28: pre-pull buffs

        // Kit out bots with level-appropriate gear and spells via the playerbots
        // factory. Without this a freshly-created bot is naked and can never
        // complete a kill. Also catches bots whose quest-reward gear was wrong
        // (broken reward selection) — the factory's incremental equip fills empty
        // slots and replaces inferior items. Gated by starterKitDone (once per
        // process, not persisted) so login churn doesn't re-run the expensive
        // factory every blip.
        if (!rec.starterKitDone)
        {
            _bridge->EnsureStarterGear(rec.guid);
            _bridge->AutoSpecTalents(rec.guid);
            rec.starterKitDone = true;
        }

        // Ensure bags at ANY level (once per process). A 16-slot backpack fills with
        // loot and then reward-granting quest turn-ins silently fail (no room for the
        // reward) -> the bot loops forever at the ender. Observed: a hunter stuck on
        // "The Troll Cave" (q182), bags 15/16, no bags equipped. Non-destructive.
        if (!rec.bagsEnsured && sConfigMgr->GetOption<bool>("IdleBot.GiveBags", true))
        {
            _bridge->EnsureBags(rec.guid);
            rec.bagsEnsured = true;
        }

        // Per-class strategy configuration and combat parameter overrides.
        // Class routine sets combat positioning (+ranged/+close) and any
        // class-specific strategies; it also populates rec.classConfig with
        // per-class aoeThreshold / restManaPct / etc. overrides that the
        // kill-step state machine uses instead of the global config values.
        {
            CombatContext cc;
            _bridge->GetCombatContext(rec.guid, cc);
            uint8_t const classId = _bridge->GetClass(rec.guid);
            auto routine = IdleBotClassRoutine::Make(classId);
            routine->ConfigureStrategies(_bridge.get(), rec.guid, cc);
            rec.classConfig = routine->GetConfig();
            LOG_DEBUG("module.idlebot",
                "[IdleBot] bot '{}': class={} ({}) aoeThreshold={} restManaPct={} maxPullOverride={}",
                rec.name, classId, routine->ClassName(),
                rec.classConfig.aoeThreshold,
                rec.classConfig.restBeforePullManaPct,
                rec.classConfig.maxPullOverride);
        }

        // Discover nearby flight paths (flight masters auto-teach on interact).
        {
            BotPosition fmPos;
            uint64_t fmGuid = 0;
            if (_bridge->FindNearestServiceNpc(rec.guid, 0x2000 /*UNIT_NPC_FLAG_FLIGHTMASTER*/,
                30.f, fmPos, fmGuid) && fmGuid != 0)
            {
                _bridge->InteractWithNpc(rec.guid, fmGuid);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': discovered flight path from nearby flight master.",
                    rec.name);
            }
        }

        rec.strategiesEnsured = true;
        LOG_DEBUG("module.idlebot", "[IdleBot] bot '{}': ensured strategies.", rec.name);
    }

    // Organic mode: idlebot supervises while playerbots' autonomous AI does the
    // questing. We enable the same strategy set the random/free bots use to
    // auto-pick quests, travel to givers/objectives (DB-derived via TravelMgr),
    // fight, and turn in — "new rpg" owns quest selection + leveling/teleport,
    // "grind" handles killing, "loot" + class positioning come from
    // EnsureStrategies. idlebot issues no movement/combat of its own here; it
    // just keeps death handling, the IdleRPG feed, and (later) Zygor-route zone
    // nudging running. Zygor routes are a reference for where to level next, not
    // a script the bot must follow.
    bool IdleBotManager::TickOrganic(BotRecord& rec)
    {
        BotLiveStatus st;
        if (!_bridge->GetLiveStatus(rec.guid, st) || !st.online || !st.controlled)
        {
            // Re-apply strategies/quest-first on the next login (they live on the
            // per-session PlayerbotAI and are lost when the bot logs out).
            rec.organicStrategiesEnsured = false;
            return true;
        }

        // (Re)assert the autonomous setup. These live on the per-session
        // PlayerbotAI and are WIPED if the AI is recreated (e.g. the no-AI
        // self-heal re-add runs ResetStrategies), so a one-time apply isn't
        // enough — re-assert every ~15s so it self-heals. ChangeStrategy /
        // SetForceActive are idempotent.
        // NOTE: deliberately NO blanket "+flee" — it flees at 25% HP / when
        // outnumbered, which a low-geared leveling bot hits in normal quest
        // fights, so it never finishes kills (plateaus). Survival comes from
        // death recovery (organic bots don't death-pause) + hub steering.
        // Don't re-assert +new rpg while on a vendor trip (HandleVendorTrip turns
        // it off so she heads to the merchant instead of being pulled back to
        // questing); force-active/grind/loot persist regardless.
        if (!rec.maintaining && (!rec.organicStrategiesEnsured || (rec.dbgThrottle % 15 == 0)))
        {
            _bridge->SetForceActive(rec.guid, true);   // bypass BotActiveAlone throttle
            // Quest-first (DO_QUEST weight 100, RPG_GRIND 0) makes a bot pursue quests
            // and never autonomously seek mobs. That works for an established bot, but a
            // freshly-created low-level bot whose quest-objective navigation can't make
            // progress just wanders ("wander-npc") forever and gains no XP. So below the
            // early-game hump, leave quest-first OFF: the default NewRpg weights include
            // RPG_GRIND, letting the bot seek and kill nearby mobs to level up. Once it is
            // established (L5+), switch to quest-first like the proven Idlebot profile.
            _bridge->SetQuestFirst(rec.guid, st.level >= 5);
            _bridge->SetNonCombatStrategy(rec.guid, "+grind");
            _bridge->SetNonCombatStrategy(rec.guid, "+new rpg");
            _bridge->SetNonCombatStrategy(rec.guid, "+loot");

            if (!rec.organicStrategiesEnsured)
            {
                rec.organicStrategiesEnsured = true;
                // Starter gear/spells/talents for fresh low-level bots is applied
                // once per session by EnsureStrategies (runs for both modes).
                EmitEvent(rec, "GUIDE", "switched to organic mode — questing autonomously");
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': organic mode active (+new rpg +grind +loot).", rec.name);
            }
        }

        // Auto-turn-in completed quests. The autonomous "new rpg" only turns a
        // quest in when it happens to interact with the ender NPC, and it often
        // fails to walk the bot back to scattered enders — so finished quests pile
        // up (objectives done, status COMPLETE) and the bot plateaus. She earned
        // them; reward them so the log frees up and leveling continues. Checked
        // every few ticks; idempotent (a quest can only be rewarded once).
        if (rec.dbgThrottle % 5 == 0)
        {
            for (uint32_t qid : _bridge->GetCompletedQuests(rec.guid))
            {
                if (_bridge->TurnInQuest(rec.guid, qid, 0))
                    EmitEvent(rec, "QUEST", Acore::StringFormat("completed and turned in quest {}", qid));
            }
        }

        // On level-up, auto-spend talents (applies directly — no trainer needed;
        // idlebot bots otherwise stay untalented since they skip randomization).
        if (st.level > rec.lastSpeccedLevel)
        {
            if (_bridge->AutoSpecTalents(rec.guid))
                EmitEvent(rec, "LEVEL", Acore::StringFormat("spent talents for level {}", st.level));
            rec.lastSpeccedLevel = st.level;
            PersistProgress(rec);
        }

        InventoryStatus const inv = _bridge->GetInventoryStatus(rec.guid);

        bool onVendorTrip = false;
        if (_vendorFreeMaintenance)
        {
            // Magic upkeep (opt-in): repair + restock anywhere, no travel. Fast but
            // not player-like. Train/sell still fire opportunistically near NPCs.
            if (rec.dbgThrottle % 60 == 0)
            {
                _bridge->Maintenance(rec.guid);
                _bridge->Train(rec.guid);
                _bridge->VendorTrash(rec.guid);
            }
        }
        else
        {
            // Player-like: run to a real merchant/repair NPC and use it when gear
            // is worn or bags are full. Owns movement while active.
            onVendorTrip = HandleVendorTrip(rec, st, inv);
        }

        std::string const act = _bridge->GetRpgActivity(rec.guid);

        // Zone direction / anti-stray: send her to a level-appropriate hub (a town,
        // so vendors/trainers are on hand) when she's out of quests for a stretch,
        // OR when her bags are nearly full so she can offload junk. Reference, not a
        // leash — once there the quest-first AI takes over again.
        if (rec.hubSteerCooldown > 0)
            --rec.hubSteerCooldown;

        bool const stalled = (st.questCount == 0) && (act == "idle" || act == "rest");
        rec.strayTicks = stalled ? rec.strayTicks + 1 : 0;
        bool const bagsFull = inv.valid && inv.freeSlots <= 2;

        // Hub-steer a stalled bot to its level/race-appropriate zone. Low levels
        // (1-19) use race-specific starting/second-zone hubs (its own race's zone);
        // 20+ uses race-neutral hubs. NextHubFor picks the best match for
        // (faction, race, level); below the lowest hub it returns false and the bot
        // quests in place.
        if (!onVendorTrip && (rec.strayTicks > 60 || bagsFull) && rec.hubSteerCooldown == 0)
        {
            LevelHub hub;
            if (NextHubFor(_bridge->GetTeamId(rec.guid), _bridge->GetRace(rec.guid), st.level, hub))
            {
                float const dx = st.pos.x - hub.x;
                float const dy = st.pos.y - hub.y;
                bool const farFromHub = !st.pos.valid || st.pos.mapId != hub.mapId ||
                    (dx * dx + dy * dy) > (400.f * 400.f);
                if (farFromHub)
                {
                    _bridge->MoveTo(rec.guid, hub.mapId, hub.x, hub.y, hub.z, 15.f);
                    EmitEvent(rec, "TRAVEL", Acore::StringFormat(
                        "{} — heading to {} (L{}+ hub)",
                        bagsFull ? "bags full" : "out of quests", hub.zone, hub.minLevel));
                    LOG_INFO("module.idlebot",
                        "[IdleBot][organic] {} steered to hub '{}' (map {} {:.0f},{:.0f}) at L{}",
                        rec.name, hub.zone, hub.mapId, hub.x, hub.y, st.level);
                }
            }
            rec.strayTicks = 0;
            rec.hubSteerCooldown = 120;  // give her ~2 min to work the hub before re-steering
        }

        // Periodic visibility (every ~5s, ungated so it's always trackable):
        // level, hp, quest count, what the autonomous AI is doing (quest vs grind
        // vs wander), and where she is.
        if (rec.dbgThrottle++ % 5 == 0)
        {
            LOG_INFO("module.idlebot",
                "[IdleBot][organic] {} L{} hp={}% quests={} doing={} bags={}/{} dura={}% stray={} pos=({:.0f},{:.0f}) map={}",
                rec.name, st.level,
                st.maxHealth ? (st.health * 100u / st.maxHealth) : 0u,
                st.questCount, act, inv.freeSlots, inv.totalSlots, inv.lowestDurabilityPct,
                rec.strayTicks,
                st.pos.valid ? st.pos.x : 0.f, st.pos.valid ? st.pos.y : 0.f,
                st.pos.mapId);
        }
        return true;
    }

    // Player-like town trip: when gear is worn, bags are full, or a level was
    // gained, stop questing and run to the relevant NPC — a repair-capable
    // merchant (repair + sell), then the class trainer (learn spells) — then
    // resume. Returns true while the trip is active (it owns movement this tick).
    bool IdleBotManager::HandleVendorTrip(BotRecord& rec, BotLiveStatus const& st, InventoryStatus const& inv)
    {
        constexpr uint32_t kRepair = 0x1000;        // UNIT_NPC_FLAG_REPAIR
        constexpr uint32_t kVendor = 0x80;          // UNIT_NPC_FLAG_VENDOR
        constexpr uint32_t kTrainerClass = 0x20;    // UNIT_NPC_FLAG_TRAINER_CLASS
        constexpr float kInRange2 = 64.f;           // ~8 yd, squared
        constexpr uint32_t kTrainDrift = 4;         // levels behind before a capital trip
        bool const needRepair = inv.valid && (inv.needsRepair || inv.lowestDurabilityPct < 35);
        bool const needSell   = inv.valid && inv.freeSlots <= 2;
        bool const needTrain  = st.level > rec.lastTrainedLevel;   // gained a level -> learn spells

        // Deliberate trainer trip: when a bot has drifted several levels without
        // learning new spells, head to its faction capital's class trainer (always
        // present, plus vendors/repair) — exactly the trip a real player makes after
        // a long stretch of questing. We never aimlessly hunt a trainer; the
        // destination is a known location (IdleBotTrainers). Gated on level >= 10 so
        // low-level bots (few/no spells to learn) just quest, and on having a pinned
        // location for the class (Death Knights start trained -> no entry -> no trip).
        TrainerLoc tloc;
        bool const haveTrainerLoc =
            ClassTrainerLoc(_bridge->GetTeamId(rec.guid), _bridge->GetClass(rec.guid), tloc);
        bool const needTrainTrip = haveTrainerLoc && st.level >= 10
                                   && (st.level - rec.lastTrainedLevel) >= kTrainDrift;

        if (!rec.maintaining)
        {
            if (!needRepair && !needSell && !needTrainTrip)
                return false;
            rec.maintaining = true;
            rec.maintTicks = 0;
            _bridge->SetNonCombatStrategy(rec.guid, "-new rpg");  // stop questing for the trip
            EmitEvent(rec, "TOWN", needRepair ? "gear's worn — heading to town"
                                  : needSell  ? "bags full — heading to town"
                                  : Acore::StringFormat("time to train — heading to {}", tloc.city));
            return true;
        }

        bool const doneRepair = !inv.valid || (!inv.needsRepair && inv.lowestDurabilityPct >= 90);
        bool const doneSell   = !inv.valid || inv.freeSlots >= 6;

        // Safety: couldn't reach an NPC in ~6.5 min — patch up, mark trained (retry
        // next drift), resume so we never get stuck.
        if (++rec.maintTicks > 400)
        {
            _bridge->Maintenance(rec.guid);
            _bridge->VendorTrash(rec.guid);
            rec.lastTrainedLevel = st.level;
            PersistProgress(rec);
            rec.maintaining = false;
            _bridge->SetNonCombatStrategy(rec.guid, "+new rpg");
            EmitEvent(rec, "TOWN", "couldn't reach an NPC — patched up and resuming");
            return false;
        }

        BotPosition const pos = _bridge->GetPosition(rec.guid);

        // PRIORITY 1 — repair/sell at a real merchant. Run to the nearest one; if
        // none is in range, steer to the level-hub town (vendors live there).
        if (!doneRepair || !doneSell)
        {
            BotPosition npos;
            uint64_t nguid = 0;
            if (_bridge->FindNearestServiceNpc(rec.guid, kRepair | kVendor, 600.f, npos, nguid) && npos.valid)
            {
                float const dx = pos.x - npos.x, dy = pos.y - npos.y;
                if (pos.valid && pos.mapId == npos.mapId && (dx * dx + dy * dy) <= kInRange2)
                {
                    _bridge->Repair(rec.guid);
                    _bridge->VendorTrash(rec.guid);
                }
                else
                    _bridge->MoveTo(rec.guid, npos.mapId, npos.x, npos.y, npos.z, 4.f);
            }
            else
            {
                LevelHub hub;
                if (NextHubFor(_bridge->GetTeamId(rec.guid), _bridge->GetRace(rec.guid), st.level, hub))
                {
                    if (!pos.valid || pos.mapId != hub.mapId)
                        _bridge->MoveTo(rec.guid, hub.mapId, hub.x, hub.y, hub.z, 15.f);
                    else
                        _bridge->MoveTo(rec.guid, hub.mapId, hub.x, hub.y, hub.z, 8.f);
                }
            }
            return true;
        }

        // PRIORITY 2 — training (repair/sell are done). Prefer a trainer in the
        // current town (no travel); otherwise, if we've drifted, make the deliberate
        // trip to the capital trainer.
        if (needTrain)
        {
            BotPosition tnpos;
            uint64_t tnguid = 0;
            bool const localTrainer =
                pos.valid
                && _bridge->FindNearestServiceNpc(rec.guid, kTrainerClass, 600.f, tnpos, tnguid)
                && tnpos.valid && tnpos.mapId == pos.mapId;

            if (localTrainer)
            {
                float const dx = pos.x - tnpos.x, dy = pos.y - tnpos.y;
                if ((dx * dx + dy * dy) <= kInRange2)
                {
                    _bridge->LearnAvailableSpells(rec.guid);
                    rec.lastTrainedLevel = st.level;          // fall through to finish
                    PersistProgress(rec);
                }
                else
                {
                    _bridge->MoveTo(rec.guid, tnpos.mapId, tnpos.x, tnpos.y, tnpos.z, 4.f);
                    return true;
                }
            }
            else if (needTrainTrip)
            {
                // No trainer here and we've drifted -> go to the capital trainer.
                if (!pos.valid || pos.mapId != tloc.mapId)
                {
                    _bridge->MoveTo(rec.guid, tloc.mapId, tloc.x, tloc.y, tloc.z, 15.f);
                    return true;
                }
                float const dx = pos.x - tloc.x, dy = pos.y - tloc.y;
                if ((dx * dx + dy * dy) > kInRange2)
                {
                    _bridge->MoveTo(rec.guid, tloc.mapId, tloc.x, tloc.y, tloc.z, 4.f);
                    return true;
                }
                _bridge->LearnAvailableSpells(rec.guid);
                rec.lastTrainedLevel = st.level;              // arrived -> finish
                PersistProgress(rec);
                EmitEvent(rec, "TOWN", Acore::StringFormat("trained at {}", tloc.city));
            }
            else
            {
                // Small drift, no trainer in this town: don't chase it. Mark so we
                // don't loop; we'll catch up on the next town visit or capital trip.
                rec.lastTrainedLevel = st.level;
                PersistProgress(rec);
            }
        }

        rec.maintaining = false;
        _bridge->SetNonCombatStrategy(rec.guid, "+new rpg");  // resume questing
        EmitEvent(rec, "TOWN", "done in town — back to questing");
        return false;
    }

    // Poll level/quest/inventory deltas and emit IdleRPG events on change (Priority 6).
    void IdleBotManager::PollDeltas(BotRecord& rec)
    {
        BotLiveStatus st;
        if (!_bridge->GetLiveStatus(rec.guid, st) || !st.online)
            return;

        InventoryStatus inv = _bridge->GetInventoryStatus(rec.guid);

        if (!rec.deltasInitialized)
        {
            rec.lastLevel = st.level;
            rec.lastQuestCount = st.questCount;
            rec.lastFreeSlots = inv.valid ? inv.freeSlots : 0;
            rec.deltasInitialized = true;
            return;
        }

        if (st.level > rec.lastLevel)
        {
            EmitEvent(rec, "LEVEL", Acore::StringFormat("reached level {}", st.level));
            rec.lastLevel = st.level;
        }

        // Quest log shrank → a turn-in completed (the executor logs accepts/turn-ins
        // by step; this catches autonomous turn-ins too). Growth is already logged.
        if (st.questCount < rec.lastQuestCount)
            EmitEvent(rec, "QUEST", Acore::StringFormat("quest log now {} active", st.questCount));
        rec.lastQuestCount = st.questCount;

        if (inv.valid)
            rec.lastFreeSlots = inv.freeSlots;
    }

    bool IdleBotManager::QuestObjectiveProgress(BotRecord& rec, GuideStep const& step, uint32_t& outCurrent, uint32_t& outRequired) const
    {
        outCurrent = 0;
        outRequired = 0;

        uint32_t questId = 0;
        uint8_t objectiveIndex = 0;
        if (!ParseQuestObjectiveCondition(step.completionCondition, questId, objectiveIndex))
            return false;

        bool const hasBridgeProgress = _bridge->GetQuestObjectiveProgress(rec.guid, questId, objectiveIndex, outCurrent, outRequired);

        if (step.type == StepType::KillMobs && !step.creatureIds.empty())
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            bool const isKillObjective = quest && quest->RequiredNpcOrGo[objectiveIndex] != 0;

            // Observed-loot fallback: count corpses we looted for this kill step.
            // ONLY when the bridge can't read the real kill credit (!hasBridgeProgress)
            // — otherwise trust the authoritative count. Corpses-looted OVER-counts
            // real credit (AoE/contested kills shared with another bot, looting a
            // corpse we didn't tag), so letting it satisfy completion advances the
            // step early and then the turn-in stalls forever on an INCOMPLETE quest
            // (observed in the wild: two same-zone bots on one kill quest). Gating on
            // !hasBridgeProgress keeps the fallback for its real purpose (bridge can't
            // resolve the objective at all) without faking progress. Still requires
            // the bot to actually hold the quest.
            QuestState const qs = _bridge->GetQuestStatus(rec.guid, questId);
            if (isKillObjective && !hasBridgeProgress &&
                (qs == QuestState::InProgress || qs == QuestState::Complete))
                outCurrent = std::max<uint32_t>(outCurrent, rec.observedKillLootsCurrentStep);

            return hasBridgeProgress || outRequired > 0;
        }

        return hasBridgeProgress;
    }

    bool IdleBotManager::CompletionConditionMet(BotRecord& rec, GuideStep const& step, uint32_t* outCurrent, uint32_t* outRequired) const
    {
        // has_item:<itemId>/<count> — done when bot has enough of an item in bags.
        // Used for intermediate crafting steps (collect before use-at-forge, etc.).
        static constexpr std::string_view kHasItem = "has_item:";
        if (step.completionCondition.rfind(kHasItem.data(), 0) == 0)
        {
            std::string const payload = step.completionCondition.substr(kHasItem.size());
            std::size_t const slash = payload.find('/');
            if (slash != std::string::npos)
            {
                try
                {
                    uint32_t const itemId  = static_cast<uint32_t>(std::stoul(payload.substr(0, slash)));
                    uint32_t const needed  = static_cast<uint32_t>(std::stoul(payload.substr(slash + 1)));
                    uint32_t const have    = _bridge->GetItemCount(rec.guid, itemId, false);
                    if (outCurrent)  *outCurrent  = have;
                    if (outRequired) *outRequired = needed;
                    return have >= needed;
                }
                catch (...) {}
            }
        }

        uint32_t current = 0;
        uint32_t required = 0;

        bool const hasObjectiveProgress = QuestObjectiveProgress(rec, step, current, required);

        if (outCurrent)
            *outCurrent = current;
        if (outRequired)
            *outRequired = required;

        if (step.questId.has_value())
        {
            QuestState const qs = _bridge->GetQuestStatus(rec.guid, *step.questId);
            if (qs == QuestState::Complete || qs == QuestState::Rewarded)
                return true;
        }

        if (hasObjectiveProgress)
            return required > 0 && current >= required;

        if (step.questId.has_value())
        {
            QuestState const qs = _bridge->GetQuestStatus(rec.guid, *step.questId);
            return qs == QuestState::Complete || qs == QuestState::Rewarded;
        }

        return false;
    }

    bool IdleBotManager::StepAppliesToBot(BotRecord const& rec, GuideStep const& step) const
    {
        if (!_bridge || !rec.guid)
            return true;

        if (step.raceMask.has_value())
        {
            uint8_t const race = _bridge->GetRace(rec.guid);
            if (race == 0 || ((*step.raceMask) & (1u << (race - 1))) == 0)
                return false;
        }

        if (step.classMask.has_value())
        {
            uint8_t const klass = _bridge->GetClass(rec.guid);
            if (klass == 0 || ((*step.classMask) & (1u << (klass - 1))) == 0)
                return false;
        }

        if (step.factionMask.has_value())
        {
            uint8_t const team = _bridge->GetTeamId(rec.guid);
            uint32_t const teamMask = (team == TEAM_ALLIANCE) ? 0x1u : 0x2u;
            if (((*step.factionMask) & teamMask) == 0)
                return false;
        }

        return true;
    }

    bool IdleBotManager::StepHasCoordinates(GuideStep const& step) const
    {
        return step.coords.x != 0.f || step.coords.y != 0.f || step.coords.z != 0.f;
    }

    bool IdleBotManager::MoveToStepPosition(BotRecord& rec, GuideStep const& step, float minRadius) const
    {
        if (!_bridge || !rec.guid || !StepHasCoordinates(step))
            return false;

        float const radius = std::max(step.coords.radius, minRadius);
        BotPosition const pos = _bridge->GetPosition(rec.guid);
        if (!pos.valid)
            return true;
        if (pos.mapId == step.coords.mapId)
        {
            float const dx = pos.x - step.coords.x;
            float const dy = pos.y - step.coords.y;
            float const dz = pos.z - step.coords.z;
            if ((dx * dx + dy * dy + dz * dz) <= (radius * radius))
                return false;
        }

        _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, radius);
        return true;
    }

    bool IdleBotManager::RewindToQuestAcceptStep(BotRecord& rec, Guide const& guide, uint32_t questId, char const* reason)
    {
        if (!_bridge || !rec.guid)
            return false;

        QuestState const qs = _bridge->GetQuestStatus(rec.guid, questId);
        if (qs != QuestState::NotStarted && qs != QuestState::Unknown)
            return false;

        for (uint32_t i = 0; i < guide.steps.size(); ++i)
        {
            GuideStep const& candidate = guide.steps[i];
            if (candidate.type != StepType::AcceptQuest || !candidate.questId.has_value() ||
                *candidate.questId != questId || !StepAppliesToBot(rec, candidate))
                continue;

            if (i >= rec.currentStepIndex)
                return false;

            rec.currentStepIndex = i;
            rec.deathCountStep = 0;
            rec.rescueRelocateCount = 0;
            rec.stepState = "idle";
            rec.observedKillLootsCurrentStep = 0;
            rec.lastObservedKillLootGuid = 0;
            ResetObjectStepState(rec);
            PersistProgress(rec);

            EmitEvent(rec, "QUEST", Acore::StringFormat(
                "rewound to accept quest {} at step {}/{} ({})",
                questId, i + 1, guide.steps.size(), reason ? reason : "missing quest"));
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': rewound to step {}/{} for quest {} ({}).",
                rec.name, i + 1, guide.steps.size(), questId, reason ? reason : "missing quest");
            return true;
        }

        return false;
    }

    uint32_t IdleBotManager::FastForwardRewardedSteps(BotRecord& rec, Guide const& guide, char const* reason)
    {
        if (!_bridge)
            return 0;

        uint32_t skipped = 0;
        while (rec.currentStepIndex < guide.steps.size())
        {
            GuideStep const& step = guide.steps[rec.currentStepIndex];
            if (!step.questId.has_value())
                break;

            QuestState const qs = _bridge->GetQuestStatus(rec.guid, *step.questId);
            if (qs != QuestState::Rewarded)
                break;

            _bridge->NormalizeRewardedQuestState(rec.guid, *step.questId);
            ++rec.currentStepIndex;
            ++skipped;
        }

        if (skipped > 0)
        {
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': fast-forwarded {} rewarded step(s) in guide '{}' ({}).",
                rec.name, skipped, rec.guideId, reason ? reason : "rewarded-prefix");
        }

        return skipped;
    }

    // Recover a turn-in that's waiting on a quest the bot HOLDS but hasn't actually
    // completed (status InProgress, not Complete). Rewinds to the quest's accept step
    // so its objective steps re-run — used when an objective advanced early (e.g. a
    // kill step credited via contested/AoE corpses, or quest items were lost after
    // the collect step completed). Only rewinds when the guide actually has objective
    // steps for this quest to redo; otherwise returns false (a quest emitted as
    // accept->turn-in with no objective step we can drive must NOT loop here).
    bool IdleBotManager::RewindToQuestObjectives(BotRecord& rec, Guide const& guide, uint32_t questId, char const* reason)
    {
        if (!_bridge || !rec.guid)
            return false;

        uint32_t firstStep = guide.steps.size();
        bool hasObjective = false;
        // Include the step immediately before the current turn-in step; many quest
        // routes place the final objective directly adjacent to the turn-in.
        uint32_t const limit = std::min<uint32_t>(rec.currentStepIndex + 1, guide.steps.size());
        for (uint32_t i = 0; i < limit; ++i)
        {
            GuideStep const& c = guide.steps[i];
            if (!c.questId.has_value() || *c.questId != questId || !StepAppliesToBot(rec, c))
                continue;
            bool const isDiscoveryObjective =
                c.type == StepType::MoveTo &&
                (!c.completionCondition.empty() || c.areaTrigger.has_value());
            bool const isQuestObjectiveStep =
                c.type != StepType::AcceptQuest &&
                c.type != StepType::TurnInQuest &&
                c.type != StepType::Conditional &&
                c.type != StepType::Checkpoint &&
                c.type != StepType::Fallback;
            if (c.type == StepType::AcceptQuest || isQuestObjectiveStep || isDiscoveryObjective)
            {
                if (i < firstStep)
                    firstStep = i;
                if (c.type != StepType::AcceptQuest)
                    hasObjective = true;
            }
        }

        if (!hasObjective || firstStep >= guide.steps.size())
            return false;

        rec.currentStepIndex = firstStep;
        rec.deathCountStep = 0;
        rec.rescueRelocateCount = 0;
        rec.stepState = "idle";
        rec.observedKillLootsCurrentStep = 0;
        rec.lastObservedKillLootGuid = 0;
        ResetObjectStepState(rec);
        PersistProgress(rec);

        EmitEvent(rec, "QUEST", Acore::StringFormat(
            "rewound to redo quest {} objectives at step {}/{} ({})",
            questId, firstStep + 1, guide.steps.size(), reason ? reason : "incomplete at turn-in"));
        LOG_INFO("module.idlebot",
            "[IdleBot] bot '{}': rewound to step {}/{} to redo quest {} objectives ({}).",
            rec.name, firstStep + 1, guide.steps.size(), questId, reason ? reason : "incomplete at turn-in");
        return true;
    }

    void IdleBotManager::RoamKillObjective(BotRecord& rec, GuideStep const& step)
    {
        // Try to find a quest creature first — move toward it directly.
        BotPosition center;
        center.mapId = step.coords.mapId;
        center.x = step.coords.x;
        center.y = step.coords.y;
        center.z = step.coords.z;
        center.valid = true;

        if (!step.creatureIds.empty())
        {
            BotPosition targetPos;
            uint64_t targetGuid = 0;
            float searchRadius = step.coords.radius;
            if (searchRadius < 120.f)
                searchRadius = 120.f;
            if (searchRadius > 220.f)
                searchRadius = 220.f;

            if (_bridge->FindNearestQuestCreature(rec.guid, step.creatureIds, center, searchRadius, targetPos, targetGuid) &&
                targetPos.valid)
            {
                _bridge->MoveTo(rec.guid, targetPos.mapId, targetPos.x, targetPos.y, targetPos.z, 5.f);
                return;
            }
        }

        // Hotspot patrol: cycle through defined waypoints instead of random scatter.
        if (!step.hotspots.empty())
        {
            ++rec.hotspotTicks;
            // Advance to next hotspot after 60 ticks (~1 min) or if no targets here.
            if (rec.hotspotTicks > 60)
            {
                rec.currentHotspot = (rec.currentHotspot + 1) % step.hotspots.size();
                rec.hotspotTicks = 0;
            }
            auto const& hs = step.hotspots[rec.currentHotspot];
            _bridge->MoveTo(rec.guid, hs.mapId, hs.x, hs.y, hs.z, 10.f);
            return;
        }

        float spread = step.coords.radius > 0.f ? step.coords.radius * 0.35f : 30.f;
        if (spread > 60.f)
            spread = 60.f;
        if (spread < 10.f)
            spread = 10.f;

        _bridge->MoveTo(rec.guid, step.coords.mapId,
            step.coords.x + frand(-spread, spread),
            step.coords.y + frand(-spread, spread), step.coords.z, 5.f);
    }

    void IdleBotManager::StartBotSession(BotRecord& rec, bool incrementResetId)
    {
        rec.soakRunId = _soakRunId;
        if (incrementResetId || rec.resetId == 0)
            ++rec.resetId;
        rec.botSessionId = Acore::StringFormat("{}:{}:{}", _soakRunId, rec.name, ++_botSessionSerial);
    }

    void IdleBotManager::ClearBlockedState(BotRecord& rec)
    {
        rec.blockedReason.clear();
        rec.blockedSince.clear();
        rec.lastFailureCode.clear();
        rec.requiresUserAction = false;
        if (rec.stepState == "blocked" || rec.stepState == "paused")
            rec.stepState = "idle";
    }

    void IdleBotManager::BlockBot(BotRecord& rec, char const* failureCode, std::string const& message, bool requiresUserAction)
    {
        rec.paused = true;
        rec.stepState = "blocked";
        rec.blockedReason = failureCode ? failureCode : "BLOCKED";
        if (rec.blockedSince.empty())
            rec.blockedSince = UtcTimestampString();
        rec.lastFailureCode = failureCode ? failureCode : "";
        rec.requiresUserAction = requiresUserAction;
        EmitEvent(rec, "FAILURE", message, failureCode);
        PersistProgress(rec);
        WriteLiveState(rec);
    }

    bool IdleBotManager::IsInsideObjectiveArea(GuideStep const& step, BotPosition const& pos, float* outDistance) const
    {
        if (!pos.valid || pos.mapId != step.coords.mapId)
        {
            if (outDistance)
                *outDistance = -1.f;
            return false;
        }

        float const dx = pos.x - step.coords.x;
        float const dy = pos.y - step.coords.y;
        float const dz = pos.z - step.coords.z;
        float const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (outDistance)
            *outDistance = dist;
        return dist <= step.coords.radius;
    }

    // Emit a categorized IdleRPG event to the per-bot log and (optionally) the
    // idlebot_events table. bot_id is resolved by subselect to stay decoupled.
    void IdleBotManager::EmitEvent(const BotRecord& rec, const char* category, const std::string& message, const char* eventCode)
    {
        sIdleBotLog->Write(rec.name, category, message);
        if (_eventsToDb)
        {
            CharacterDatabase.Execute(
                "INSERT INTO idlebot_events (bot_id, event_type, event_code, soak_run_id, bot_session_id, reset_id, detail, created_at) "
                "SELECT id, '{}', {}, {}, {}, {}, '{}', UTC_TIMESTAMP() FROM idlebot_bots WHERE bot_name = '{}'",
                SqlEscape(category),
                eventCode ? ("'" + SqlEscape(eventCode) + "'") : std::string("NULL"),
                rec.soakRunId.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.soakRunId) + "'"),
                rec.botSessionId.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.botSessionId) + "'"),
                rec.resetId,
                SqlEscape(message),
                SqlEscape(rec.name));
        }
    }

    // Playerlike policy guard — call before any GM-style teleport/resurrect/advance.
    // Returns false if playerlike mode forbids the action; emits POLICY_BLOCKED.
    // Returns true only when the relevant debug flag is on; emits a prominent warning.
    // See docs/PLAYERLIKE_POLICY.md for the full policy.
    bool IdleBotManager::CanUseRuntimeCheatRecovery(BotRecord const& rec, std::string_view reason)
    {
        bool const anyCheatOn = _allowCheatTeleport || _allowCheatResurrect ||
            _allowForceQuestAdvance || _allowForceSkipForSoak || _allowGMRecovery;

        if (_playlikeMode && !anyCheatOn)
        {
            // All cheat flags are off (default). Block and record.
            EmitEvent(rec, "POLICY_BLOCKED",
                Acore::StringFormat("[PLAYERLIKE_POLICY] blocked cheat recovery: {}. "
                    "Enable the relevant debug flag only for developer testing — "
                    "not during a soak. See docs/PLAYERLIKE_POLICY.md.", reason));
            return false;
        }
        // Playerlike mode off or at least one cheat flag on — allow but warn loudly.
        LOG_WARN("module.idlebot",
            "[IdleBot][CHEAT_RECOVERY_ENABLED] bot '{}' using cheat recovery for reason '{}'. "
            "This must not count as valid playerlike progression. "
            "Ensure all cheat flags are OFF before a real soak.",
            rec.name, reason);
        EmitEvent(rec, "CHEAT_RECOVERY",
            Acore::StringFormat("[CHEAT_RECOVERY_ENABLED] reason: {} — invalid progression.", reason));
        return true;
    }

    // Persist guide progress + organic leveling markers so a restart resumes
    // cleanly instead of redoing the same town training/spec work.
    void IdleBotManager::PersistProgress(const BotRecord& rec)
    {
        std::string const guideClause =
            rec.guideId.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.guideId) + "'");
        std::string const soakRunClause =
            rec.soakRunId.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.soakRunId) + "'");
        std::string const botSessionClause =
            rec.botSessionId.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.botSessionId) + "'");
        std::string const blockedReasonClause =
            rec.blockedReason.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.blockedReason) + "'");
        std::string const blockedSinceClause =
            rec.blockedSince.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.blockedSince) + "'");
        std::string const lastFailureClause =
            rec.lastFailureCode.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.lastFailureCode) + "'");
        CharacterDatabase.Execute(
            "UPDATE idlebot_bots SET guide_id = {}, step_index = {}, step_state = '{}', "
            "death_count_total = {}, death_count_current_step = {}, "
            "last_trained_level = {}, last_specced_level = {}, "
            "soak_run_id = {}, bot_session_id = {}, reset_id = {}, "
            "blocked_reason = {}, blocked_since = {}, last_failure_code = {}, requires_user_action = {} "
            "WHERE bot_name = '{}'",
            guideClause, rec.currentStepIndex, SqlEscape(rec.stepState),
            rec.deathCountTotal, rec.deathCountStep,
            rec.lastTrainedLevel, rec.lastSpeccedLevel,
            soakRunClause, botSessionClause, rec.resetId,
            blockedReasonClause, blockedSinceClause, lastFailureClause, rec.requiresUserAction ? 1 : 0,
            SqlEscape(rec.name));
    }

    // Advance to the next guide step: fresh per-step death budget + persist.
    void IdleBotManager::AdvanceStep(BotRecord& rec)
    {
        ++rec.currentStepIndex;
        rec.deathCountStep = 0;
        rec.rescueRelocateCount = 0;
        rec.stepElapsedMs = 0;
        rec.consecutiveForceSkips = 0;   // completed a real step → clear the skip streak
        rec.lastObjectiveCurrent = 0;
        rec.stuckTicks = 0;
        rec.currentHotspot = 0;
        rec.hotspotTicks = 0;
        rec.posStallTicks = 0;
        rec.unstickAttempt = 0;
        rec.classTravelTicks = 0;
        rec.blackspots.clear();
        rec.stepState = "idle";
        ClearBlockedState(rec);
        rec.observedKillLootsCurrentStep = 0;
        rec.lastObservedKillLootGuid = 0;
        ResetObjectStepState(rec);
        PersistProgress(rec);
    }

    bool IdleBotManager::SkipAllowedAtLevel(BotRecord& rec)
    {
        if (_noSkipBelowLevel == 0)
            return true;
        return _bridge->GetLevel(rec.guid) >= _noSkipBelowLevel;
    }

    void IdleBotManager::SkipQuestSteps(BotRecord& rec, Guide const& guide, uint32_t questId)
    {
        // Advance past every consecutive step that belongs to this quest (its
        // accept, objective(s) and turn-in are emitted together in generated
        // guides) so an un-acceptable quest can't wedge the rest of the guide.
        while (rec.currentStepIndex < guide.steps.size()
               && guide.steps[rec.currentStepIndex].questId.has_value()
               && *guide.steps[rec.currentStepIndex].questId == questId)
        {
            ++rec.currentStepIndex;
        }
        rec.deathCountStep = 0;
        rec.rescueRelocateCount = 0;
        rec.stepElapsedMs = 0;
        rec.lastObjectiveCurrent = 0;
        rec.stuckTicks = 0;
        rec.classTravelTicks = 0;
        rec.stepState = "idle";
        ClearBlockedState(rec);
        rec.observedKillLootsCurrentStep = 0;
        rec.lastObservedKillLootGuid = 0;
        ResetObjectStepState(rec);

        // Track consecutive force-skips. If many quests in a row are skipped due to
        // combat failure, the guide or combat is systematically broken. Quarantine the
        // bot — fake progression through broken quests is not useful data.
        ++rec.consecutiveForceSkips;
        if (rec.consecutiveForceSkips >= 6 && !rec.paused)
        {
            rec.paused = true;
            EmitEvent(rec, "FAILURE", Acore::StringFormat(
                "[STALE:FORCE_SKIP_STREAK] {} consecutive quests force-skipped (COMBAT_TOO_HARD) "
                "on step {} — quarantining bot. Combat/guide behavior is broken. "
                "Fix the root cause, then use '.idlebot resume {}'.",
                rec.consecutiveForceSkips, rec.currentStepIndex, rec.name));
            LOG_WARN("module.idlebot",
                "[IdleBot] bot '{}': QUARANTINED — FORCE_SKIP_STREAK ({} consecutive) at step {}. "
                "Other bots continue. Fix combat/pull/guide before resuming.",
                rec.name, rec.consecutiveForceSkips, rec.currentStepIndex);
        }

        PersistProgress(rec);
    }

    void IdleBotManager::ResetObjectStepState(BotRecord& rec)
    {
        rec.objectWaitMs = 0;
        rec.lastObjectRetryMs = 0;
        rec.lastObjectRoamMs = 0;
        rec.objectAttemptsCurrentStep = 0;
        rec.lastObjectGuid = 0;
        rec.lastObjectFailureReason.clear();
        rec.useItemCooldownTicks = 0;
        rec.useItemProgressCount = 0;
    }

    void IdleBotManager::RebindBotGuide(BotRecord& rec)
    {
        if (rec.guideId.empty())
            return;

        auto it = _guides.find(rec.guideId);
        if (it == _guides.end())
        {
            EmitEvent(rec, "GUIDE_RELOAD_STEP_MISSING",
                Acore::StringFormat("guide '{}' not found after reload — bot paused", rec.guideId));
            rec.paused = true;
            PersistProgress(rec);
            return;
        }

        Guide const& guide = it->second;
        if (rec.currentStepIndex >= guide.steps.size())
        {
            EmitEvent(rec, "GUIDE_RELOAD_STEP_MISSING",
                Acore::StringFormat("step {} no longer exists in guide '{}' after reload — bot paused",
                    rec.currentStepIndex, rec.guideId));
            rec.paused = true;
            PersistProgress(rec);
            return;
        }

        // Clear stale per-step cached state so the bot re-evaluates its objective.
        ResetObjectStepState(rec);
        rec.stuckTicks     = 0;
        rec.hotspotTicks   = 0;
        rec.currentHotspot = 0;
        rec.stepElapsedMs  = 0;
    }

    bool IdleBotManager::GameObjectStepSkippable(GuideStep const& step) const
    {
        return step.adaptive.optional || step.adaptive.skippable || !step.adaptive.requiredForChain;
    }

    uint32_t IdleBotManager::GameObjectMaxWaitMs(GuideStep const& step) const
    {
        if (step.adaptive.maxAttemptMinutes > 0)
            return step.adaptive.maxAttemptMinutes * 60000u;
        return GameObjectStepSkippable(step) ? _gameObjectOptionalMaxWaitMs : _gameObjectRequiredMaxWaitMs;
    }

    bool IdleBotManager::HandleCollectItemsStep(BotRecord& rec, Guide const& guide, GuideStep const& step, bool& stepDone)
    {
        (void)guide;
        stepDone = false;

        uint32_t const itemId    = step.itemId.value_or(0);
        uint32_t const itemCount = step.itemCount > 0 ? step.itemCount : 1;

        // 1. Bag check — done immediately if item already present.
        if (itemId > 0)
        {
            uint32_t const have = _bridge->GetItemCount(rec.guid, itemId, false);
            if (have >= itemCount)
            {
                LOG_INFO("module.idlebot",
                    "[IdleBot] QB_COLLECT complete bot='{}' quest={} item={} count={}/{} reason=item_count_reached",
                    rec.name, step.questId.value_or(0), itemId, have, itemCount);
                ResetObjectStepState(rec);
                rec.objectLocalBlacklist.clear();
                stepDone = true;
                return true;
            }
        }

        // 2. Quest-objective check (secondary — item count is authoritative).
        if (step.questId.has_value() && CompletionConditionMet(rec, step))
        {
            LOG_INFO("module.idlebot",
                "[IdleBot] QB_COLLECT complete bot='{}' quest={} reason=quest_objective_met",
                rec.name, *step.questId);
            ResetObjectStepState(rec);
            rec.objectLocalBlacklist.clear();
            stepDone = true;
            return true;
        }

        // Source entries: prefer sourceGameobjectEntries; fall back to gameobjectId.
        std::vector<uint32_t> goSources = step.sourceGameobjectEntries;
        if (goSources.empty() && step.gameobjectId.has_value())
            goSources.push_back(*step.gameobjectId);

        if (goSources.empty())
        {
            LOG_WARN("module.idlebot",
                "[IdleBot] QB_COLLECT failed bot='{}' quest={} item={} reason=no_sources",
                rec.name, step.questId.value_or(0), itemId);
            ResetObjectStepState(rec);
            stepDone = true;
            return true;
        }

        float const searchRadius = step.coords.radius > 0.f ? step.coords.radius : _gameObjectDefaultSearchRadius;
        float const roamRadius   = _gameObjectRoamRadius > 0.f ? _gameObjectRoamRadius : searchRadius;
        bool const skippable     = step.adaptive.optional || step.adaptive.skippable;
        uint32_t const maxWaitMs = skippable ? _gameObjectOptionalMaxWaitMs : _gameObjectRequiredMaxWaitMs;

        rec.objectWaitMs        += _tickMs;
        rec.lastObjectRetryMs   += _tickMs;
        rec.lastObjectRoamMs    += _tickMs;
        ++rec.objectAttemptsCurrentStep;

        // Expire blacklist entries.
        for (auto it = rec.objectLocalBlacklist.begin(); it != rec.objectLocalBlacklist.end(); )
        {
            if (rec.globalTick >= it->second)
                it = rec.objectLocalBlacklist.erase(it);
            else
                ++it;
        }

        // Find nearest non-blacklisted GO from any source entry.
        uint64_t foundGuid  = 0;
        uint32_t foundEntry = 0;
        for (uint32_t entry : goSources)
        {
            uint64_t guid = _bridge->FindNearestGameObjectEntry(rec.guid, entry, searchRadius);
            if (guid == 0)
                continue;
            if (rec.objectLocalBlacklist.count(guid))
                continue;
            foundGuid  = guid;
            foundEntry = entry;
            break;
        }

        if (foundGuid != 0)
        {
            if (rec.lastObjectGuid != foundGuid)
            {
                rec.lastObjectGuid = foundGuid;
                rec.lastObjectFailureReason.clear();
                uint32_t const have = itemId > 0 ? _bridge->GetItemCount(rec.guid, itemId, false) : 0;
                LOG_INFO("module.idlebot",
                    "[IdleBot] QB_COLLECT target_selected bot='{}' quest={} item={} count={}/{} go={} guid={}",
                    rec.name, step.questId.value_or(0), itemId, have, itemCount, foundEntry, foundGuid);
            }

            if (!_bridge->IsNearGameObject(rec.guid, foundEntry, 5.5f /*INTERACTION_DISTANCE*/))
            {
                // Move directly toward the specific GO rather than the general collection
                // area. Without this, the bot loops at the area center while the GO is
                // 20-50 yards away within searchRadius.
                BotPosition goPos = _bridge->GetGameObjectPosition(rec.guid, foundGuid);
                if (goPos.valid)
                    _bridge->MoveTo(rec.guid, goPos.mapId, goPos.x, goPos.y, goPos.z, 4.0f);
                else
                    _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y,
                        step.coords.z, searchRadius);
                rec.lastObjectFailureReason = "approach";
                return false;
            }

            // Throttle interact attempts.
            if (rec.lastObjectRetryMs < _gameObjectRetryEveryMs && rec.objectAttemptsCurrentStep > 1)
                return false;

            LOG_INFO("module.idlebot",
                "[IdleBot] QB_COLLECT interact bot='{}' quest={} go={} guid={}",
                rec.name, step.questId.value_or(0), foundEntry, foundGuid);

            // Safety gate: clear nearby hostiles before interacting with the GO.
            if (!CheckGoSafety(rec, _goSafetyClearRadius))
            {
                LOG_INFO("module.idlebot",
                    "[IdleBot] {} QB_COLLECT deferred reason=hostile_near go={}",
                    rec.name, foundEntry);
                rec.posStallTicks = 0;
                return false;
            }

            bool const used = _bridge->UseGameObject(rec.guid, foundEntry, 5.5f);
            _bridge->LootNearby(rec.guid);
            rec.lastObjectRetryMs = 0;

            if (used)
            {
                uint32_t const have = itemId > 0 ? _bridge->GetItemCount(rec.guid, itemId, false) : 0;
                LOG_INFO("module.idlebot",
                    "[IdleBot] QB_COLLECT loot_attempted bot='{}' quest={} go={} item={} count={}/{}",
                    rec.name, step.questId.value_or(0), foundEntry, itemId, have, itemCount);

                if (itemId > 0 && have >= itemCount)
                {
                    LOG_INFO("module.idlebot",
                        "[IdleBot] QB_COLLECT complete bot='{}' quest={} item={} count={}/{} reason=item_count_reached",
                        rec.name, step.questId.value_or(0), itemId, have, itemCount);
                    ResetObjectStepState(rec);
                    rec.objectLocalBlacklist.clear();
                    stepDone = true;
                    return true;
                }

                // Item not acquired — blacklist this guid for 60 ticks then try another spawn.
                if (itemId > 0 && have < itemCount)
                {
                    LOG_INFO("module.idlebot",
                        "[IdleBot] QB_COLLECT retry bot='{}' quest={} item={} have={} need={} blacklisting go_guid={}",
                        rec.name, step.questId.value_or(0), itemId, have, itemCount, foundGuid);
                    rec.objectLocalBlacklist[foundGuid] = rec.globalTick + 60;
                    rec.lastObjectGuid = 0;
                }
            }
            else
            {
                // GO exists in range but is not usable (depleted herb node, wrong state, etc.).
                // Blacklist it so we stop retrying until it respawns.
                LOG_WARN("module.idlebot",
                    "[IdleBot] QB_COLLECT interact_failed bot='{}' quest={} go={} guid={} reason=go_not_usable — blacklisting",
                    rec.name, step.questId.value_or(0), foundEntry, foundGuid);
                rec.objectLocalBlacklist[foundGuid] = rec.globalTick + 60;
                rec.lastObjectGuid = 0;
            }

            return false;
        }

        // No valid source found.
        if (maxWaitMs > 0 && rec.objectWaitMs >= maxWaitMs && skippable)
        {
            LOG_INFO("module.idlebot",
                "[IdleBot] QB_COLLECT timeout bot='{}' quest={} item={} reason=optional_timeout",
                rec.name, step.questId.value_or(0), itemId);
            ResetObjectStepState(rec);
            rec.objectLocalBlacklist.clear();
            stepDone = true;
            return true;
        }

        if (rec.lastObjectRetryMs >= _gameObjectRetryEveryMs)
        {
            uint32_t const have = itemId > 0 ? _bridge->GetItemCount(rec.guid, itemId, false) : 0;
            LOG_INFO("module.idlebot",
                "[IdleBot] QB_COLLECT waiting bot='{}' quest={} item={} count={}/{} sources=[{}] waitMs={}",
                rec.name, step.questId.value_or(0), itemId, have, itemCount,
                goSources.empty() ? 0u : goSources.front(), rec.objectWaitMs);
            rec.lastObjectRetryMs = 0;
        }

        // Suppress unstick while intentionally waiting for GO respawn.
        rec.posStallTicks = 0;

        // Fight back if attacked while waiting — mobs will kill a passive roaming bot.
        {
            BotPosition threatPos;
            uint64_t threatGuid = 0;
            if (_bridge->FindNearestHostile(rec.guid, _goSafetyClearRadius, threatPos, threatGuid) && threatGuid != 0)
            {
                _bridge->AttackCreature(rec.guid, threatGuid);
                rec.lastEngagedGuid = threatGuid;
                rec.lastEngagedWasObjective = false;
                rec.lastEngagedWasPathBlocker = false;
                return false;
            }
        }

        if (rec.lastObjectRoamMs >= _gameObjectRoamEveryMs)
        {
            LOG_INFO("module.idlebot",
                "[IdleBot] QB_WAIT suppressed_unstick bot='{}' reason=waiting_for_go_respawn entries=[{}] quest={}",
                rec.name, goSources.empty() ? 0u : goSources.front(), step.questId.value_or(0));
            rec.lastObjectRoamMs = 0;
            float const spread = roamRadius > 0.f ? roamRadius : searchRadius;
            _bridge->MoveTo(rec.guid, step.coords.mapId,
                step.coords.x + frand(-spread, spread),
                step.coords.y + frand(-spread, spread),
                step.coords.z, searchRadius);
        }
        else if (step.coords.mapId != 0)
        {
            _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, searchRadius);
        }

        // Required GOs wait indefinitely — some drops take an hour (contested spawns,
        // long respawn timers). Only optional steps have a timeout; required steps grind
        // until the bot gets what it needs. Quarantine only happens on structural failures
        // detected elsewhere (no spawn in DB, 0% drop chance, wrong coordinates).

        return false;
    }

    bool IdleBotManager::CheckGoSafety(BotRecord& rec, float clearRadius)
    {
        if (clearRadius <= 0.f || !_bridge)
            return true;
        BotPosition threatPos;
        uint64_t threatGuid = 0;
        if (!_bridge->FindNearestHostile(rec.guid, clearRadius, threatPos, threatGuid) || threatGuid == 0)
            return true;
        // Hostile within interaction radius — attack it before using the object.
        _bridge->AttackCreature(rec.guid, threatGuid);
        rec.lastEngagedGuid = threatGuid;
        rec.lastEngagedWasObjective = false;
        rec.lastEngagedWasPathBlocker = false;
        return false;
    }

    bool IdleBotManager::HandleInteractGameObjectStep(BotRecord& rec, Guide const& guide, GuideStep const& step)
    {
        (void)guide;
        bool stepDone = false;

        if (step.questId.has_value())
        {
            if (CompletionConditionMet(rec, step))
            {
                ResetObjectStepState(rec);
                return true;
            }
        }

        if (!step.gameobjectId.has_value())
        {
            LOG_WARN("module.idlebot", "[IdleBot] bot '{}': InteractGameobject step '{}' has no gameobject id — skipping.",
                rec.name, step.name);
            ResetObjectStepState(rec);
            return true;
        }

        uint32_t const goEntry = *step.gameobjectId;
        float const searchRadius = step.coords.radius > 0.f ? step.coords.radius : _gameObjectDefaultSearchRadius;
        float const roamRadius = _gameObjectRoamRadius > 0.f ? _gameObjectRoamRadius : searchRadius;
        bool const skippable = GameObjectStepSkippable(step);
        uint32_t const maxWaitMs = GameObjectMaxWaitMs(step);

        rec.objectWaitMs += _tickMs;
        rec.lastObjectRetryMs += _tickMs;
        rec.lastObjectRoamMs += _tickMs;
        ++rec.objectAttemptsCurrentStep;

        uint64_t const foundGuid = _bridge->FindNearestGameObjectEntry(rec.guid, goEntry, searchRadius);
        bool const found = foundGuid != 0;

        if (found)
        {
            if (rec.lastObjectGuid != foundGuid)
            {
                rec.lastObjectGuid = foundGuid;
                rec.lastObjectFailureReason.clear();
                EmitEvent(rec, "OBJECT", "Found object; moving to interact.");
            }

            if (!_bridge->IsNearGameObject(rec.guid, goEntry, 5.5f /*INTERACTION_DISTANCE*/))
            {
                _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z,
                    step.coords.radius > 0.f ? step.coords.radius : searchRadius);
                rec.lastObjectFailureReason = "approach";
                return false;
            }

            if (rec.lastObjectRetryMs < _gameObjectRetryEveryMs && rec.objectAttemptsCurrentStep > 1)
                return false;

            // Safety gate: clear any hostile within interaction radius before using
            // the object. A mob within 8yd will interrupt the interaction and the
            // bot will loop here; better to kill it first.
            if (!CheckGoSafety(rec, _goSafetyClearRadius))
            {
                EmitEvent(rec, "OBJECT", "Clearing hostile before object interact.");
                rec.posStallTicks = 0;
                return false;
            }

            bool const used = _bridge->UseGameObject(rec.guid, goEntry, 5.5f /*INTERACTION_DISTANCE*/);
            if (used)
                EmitEvent(rec, "OBJECT", "Used object.");
            _bridge->LootNearby(rec.guid);
            EmitEvent(rec, "LOOT", "Attempted object loot.");
            rec.lastObjectRetryMs = 0;
            rec.lastObjectFailureReason.clear();

            bool madeProgress = used;
            if (step.questId.has_value())
            {
                uint32_t objectiveCurrent = 0;
                uint32_t objectiveRequired = 0;
                if (CompletionConditionMet(rec, step, &objectiveCurrent, &objectiveRequired))
                {
                    madeProgress = true;
                    stepDone = true;
                }

                if (objectiveRequired > 0)
                {
                    EmitEvent(rec, "QUEST", Acore::StringFormat("quest progress {}/{}", objectiveCurrent, objectiveRequired));
                }
                else if (Quest const* quest = sObjectMgr->GetQuestTemplate(*step.questId))
                {
                    uint32_t currentCount = 0;
                    uint32_t requiredCount = 0;

                    if (step.itemId.has_value())
                    {
                        currentCount = _bridge->GetItemCount(rec.guid, *step.itemId, false);
                        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                        {
                            if (quest->RequiredItemId[i] == *step.itemId)
                            {
                                requiredCount = quest->RequiredItemCount[i];
                                break;
                            }
                        }
                    }

                    if (currentCount > 0 && requiredCount > 0)
                    {
                        EmitEvent(rec, "QUEST", Acore::StringFormat("quest item progress {}/{}", currentCount, requiredCount));
                        if (currentCount >= requiredCount)
                        {
                            stepDone = true;
                            madeProgress = true;
                        }
                    }
                }
            }

            if (!step.questId.has_value())
                stepDone = used;

            if (madeProgress)
                ResetObjectStepState(rec);
            return stepDone;
        }

        if (_gameObjectWaitForRespawn && maxWaitMs > 0 && rec.objectWaitMs >= maxWaitMs && skippable)
        {
            if (step.questId.has_value())
            {
                // Quest-linked — do not skip even if marked optional; quarantine.
                rec.paused = true;
                EmitEvent(rec, "FAILURE", Acore::StringFormat(
                    "[FAILURE:GUIDE_STEP_BAD] optional GO step '{}' (quest {}) timed out "
                    "after {}min — quest-linked steps must not be skipped. "
                    "Fix guide (add requiredForChain or fix GO). Use '.idlebot resume {}' after fixing.",
                    step.name, *step.questId, rec.objectWaitMs / 60000, rec.name));
                LOG_WARN("module.idlebot",
                    "[IdleBot] bot '{}': QUARANTINED — optional GO step '{}' quest {} timed out. "
                    "Fix guide and resume.",
                    rec.name, step.name, *step.questId);
                PersistProgress(rec);
                ResetObjectStepState(rec);
                return false;
            }
            EmitEvent(rec, "GUIDE", Acore::StringFormat(
                "optional GO step '{}' timed out after {}min — advancing (no quest link)",
                step.name, rec.objectWaitMs / 60000));
            ResetObjectStepState(rec);
            return true;
        }

        if (rec.lastObjectRetryMs >= _gameObjectRetryEveryMs)
        {
            EmitEvent(rec, "OBJECT", Acore::StringFormat(
                "[IdleBot] QB_OBJECT waiting bot='{}' entry={} waitMs={} reason=no_go_found",
                rec.name, goEntry, rec.objectWaitMs));
            rec.lastObjectRetryMs = 0;
        }

        if (_gameObjectWaitForRespawn && rec.lastObjectRoamMs >= _gameObjectRoamEveryMs)
        {
            EmitEvent(rec, "TRAVEL", "Roaming around objective area while waiting for object respawn.");
            rec.lastObjectRoamMs = 0;

            float const spread = roamRadius > 0.f ? roamRadius : searchRadius;
            _bridge->MoveTo(rec.guid, step.coords.mapId,
                step.coords.x + frand(-spread, spread),
                step.coords.y + frand(-spread, spread),
                step.coords.z,
                step.coords.radius > 0.f ? step.coords.radius : searchRadius);
        }
        else if (step.coords.mapId != 0)
        {
            _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z,
                step.coords.radius > 0.f ? step.coords.radius : searchRadius);
        }

        return false;
    }

    // CAST quests (SpecialFlags=32): the objective is "use the quest item on
    // creature X" (e.g. wake a Lazy Peon with a horn), NOT kill it. gen_dbguide
    // emits these as use_item_on_npc with item_id (the quest StartItem) + the
    // target creature_ids. Approach the nearest live target, drive the item-use
    // (UseItemOnTarget), and poll the quest objective until it credits. Mirrors
    // the gameobject-step state machine but targets a creature with an item.
    bool IdleBotManager::HandleUseItemOnNpcStep(BotRecord& rec, Guide const& guide, GuideStep const& step)
    {
        (void)guide;

        if (step.questId.has_value() && CompletionConditionMet(rec, step))
        {
            ResetObjectStepState(rec);
            return true;
        }

        if (!step.itemId.has_value() || step.creatureIds.empty())
        {
            LOG_WARN("module.idlebot",
                "[IdleBot] bot '{}': UseItemOnNpc step '{}' missing item_id/creature_ids — skipping.",
                rec.name, step.name);
            ResetObjectStepState(rec);
            return true;
        }

        uint32_t const itemId = *step.itemId;
        float const searchRadius = step.coords.radius > 0.f ? step.coords.radius : _gameObjectDefaultSearchRadius;

        rec.lastObjectRetryMs += _tickMs;
        ++rec.objectAttemptsCurrentStep;

        BotPosition tgtPos;
        uint64_t tgtGuid = 0;
        BotPosition const center{ step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, 0.f, true };
        if (_bridge->FindNearestQuestCreature(rec.guid, step.creatureIds, center, searchRadius, tgtPos, tgtGuid)
            && tgtGuid != 0)
        {
            // Skip a GUID that failed recently — same blacklist used for GOs.
            if (rec.objectLocalBlacklist.count(tgtGuid))
                return false;

            BotPosition const me = _bridge->GetPosition(rec.guid);
            float const dx = me.x - tgtPos.x, dy = me.y - tgtPos.y, dz = me.z - tgtPos.z;
            bool const inRange = me.valid && (dx * dx + dy * dy + dz * dz) <= (5.0f * 5.0f);
            if (!inRange)
            {
                _bridge->MoveTo(rec.guid, tgtPos.mapId, tgtPos.x, tgtPos.y, tgtPos.z, 4.0f);
                return false;
            }

            // Throttle the use cadence (item cooldown / cast time) so we don't spam.
            if (rec.lastObjectRetryMs < _gameObjectRetryEveryMs && rec.objectAttemptsCurrentStep > 1)
                return false;
            rec.lastObjectRetryMs = 0;

            // Safety gate: clear any OTHER hostile that might pull us off the target.
            // Use a smaller radius than GO safety (we're already in range of the quest NPC).
            if (!CheckGoSafety(rec, _goSafetyClearRadius * 0.75f))
            {
                EmitEvent(rec, "OBJECT", "Clearing hostile before item-on-target use.");
                return false;
            }

            bool const used = _bridge->UseItemOnTarget(rec.guid, itemId, tgtGuid);
            if (used)
                EmitEvent(rec, "OBJECT", Acore::StringFormat("Used item {} on target.", itemId));
            else
            {
                // Item use failed on this NPC (immune, wrong target, etc.) — blacklist it
                // for 60 ticks so FindNearestQuestCreature tries a different spawn.
                rec.objectLocalBlacklist[tgtGuid] = rec.globalTick + 60;
                EmitEvent(rec, "OBJECT", Acore::StringFormat("Item {} failed on target {} — blacklisted.", itemId, tgtGuid));
            }

            uint32_t cur = 0, req = 0;
            if (CompletionConditionMet(rec, step, &cur, &req))
            {
                ResetObjectStepState(rec);
                return true;
            }
            if (req > 0)
                EmitEvent(rec, "QUEST", Acore::StringFormat("quest progress {}/{}", cur, req));
            return false;
        }

        // No live target in range — move to the objective area and wait for spawns.
        _bridge->MoveTo(rec.guid, step.coords.mapId, step.coords.x, step.coords.y, step.coords.z, searchRadius);
        return false;
    }

    void IdleBotManager::LoadBots()
    {
        // Best-effort: if the idlebot_bots table is absent the query returns null
        // and we simply start with an empty registry. Restores guide progress so a
        // worldserver restart resumes mid-guide rather than from step 0 (Priority 3).
        QueryResult result = CharacterDatabase.Query(
            "SELECT bot_name, active, guide_id, step_index, step_state, "
            "death_count_total, death_count_current_step, decision_mode, "
            "last_trained_level, last_specced_level, "
            "soak_run_id, bot_session_id, reset_id, blocked_reason, blocked_since, "
            "last_failure_code, requires_user_action "
            "FROM idlebot_bots");
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
            if (!fields[2].IsNull())
            {
                std::string guideId = fields[2].Get<std::string>();
                // Only restore the guide if it's still registered; otherwise drop it.
                if (!guideId.empty() && _guides.count(guideId))
                {
                    rec.guideId = guideId;
                    rec.currentStepIndex = fields[3].Get<uint32_t>();
                    rec.stepState = fields[4].Get<std::string>();
                }
            }
            rec.deathCountTotal = fields[5].Get<uint32_t>();
            rec.deathCountStep = fields[6].Get<uint32_t>();
            if (!fields[7].IsNull())
            {
                std::string mode = fields[7].Get<std::string>();
                if (!mode.empty())
                    rec.decisionMode = mode;
            }
            rec.lastTrainedLevel = fields[8].Get<uint32_t>();
            rec.lastSpeccedLevel = fields[9].Get<uint32_t>();
            if (!fields[10].IsNull())
                rec.soakRunId = fields[10].Get<std::string>();
            if (!fields[11].IsNull())
                rec.botSessionId = fields[11].Get<std::string>();
            rec.resetId = fields[12].Get<uint32_t>();
            if (!fields[13].IsNull())
                rec.blockedReason = fields[13].Get<std::string>();
            if (!fields[14].IsNull())
                rec.blockedSince = fields[14].Get<std::string>();
            if (!fields[15].IsNull())
                rec.lastFailureCode = fields[15].Get<std::string>();
            rec.requiresUserAction = fields[16].Get<bool>();
            if (rec.stepState == "blocked" || rec.stepState == "paused" || !rec.blockedReason.empty())
                rec.paused = true;
            StartBotSession(rec, false);
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
        StartBotSession(rec, false);
        _bots.emplace(name, std::move(rec));

        sIdleBotLog->Write(name, "EVENT", "registered with idlebot");

        // Persist (best-effort, async). Character names are constrained to a safe
        // charset by the client, so direct interpolation is acceptable here.
        CharacterDatabase.Execute(
            "INSERT INTO idlebot_bots (bot_name, active, decision_mode, soak_run_id, bot_session_id, reset_id) "
            "VALUES ('{}', 1, '{}', '{}', '{}', {}) "
            "ON DUPLICATE KEY UPDATE active = 1, decision_mode = VALUES(decision_mode), "
            "soak_run_id = VALUES(soak_run_id), bot_session_id = VALUES(bot_session_id), reset_id = VALUES(reset_id)",
            name, _decisionMode, _bots[name].soakRunId, _bots[name].botSessionId, _bots[name].resetId);
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

    bool IdleBotManager::GetLivePosition(const std::string& rawName, BotPosition& out, std::string& outErr) const
    {
        out = BotPosition{};
        outErr.clear();

        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end())
        {
            outErr = "No such bot: " + name;
            return false;
        }

        if (!_bridge)
        {
            outErr = "IdleBot bridge is not available.";
            return false;
        }

        BotGuid const guid = _bridge->GetBotGuid(name);
        if (!guid)
        {
            outErr = "Character not found on this realm: " + name;
            return false;
        }

        BotLiveStatus st;
        if (!_bridge->GetLiveStatus(guid, st) || !st.online || !st.pos.valid)
        {
            outErr = "Bot is not online: " + name;
            return false;
        }

        out = st.pos;
        return true;
    }

    bool IdleBotManager::PauseBot(const std::string& rawName)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = true;
        it->second.stepState = "paused";
        it->second.requiresUserAction = false;
        PersistProgress(it->second);
        sIdleBotLog->Write(name, "EVENT", "paused");
        return true;
    }

    bool IdleBotManager::ResumeBot(const std::string& rawName)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = false;
        it->second.deathCountStep = 0;
        it->second.stepElapsedMs = 0;
        it->second.lastObjectiveCurrent = 0;
        ClearBlockedState(it->second);
        StartBotSession(it->second, true);
        PersistProgress(it->second);
        sIdleBotLog->Write(name, "EVENT", "resumed");
        return true;
    }

    bool IdleBotManager::GearBot(const std::string& rawName, std::string& outErr)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end())
        {
            outErr = "no such bot: " + name;
            return false;
        }
        BotRecord& rec = it->second;
        if (!rec.guid)
            rec.guid = _bridge->GetBotGuid(rec.name);
        if (!rec.guid)
        {
            outErr = "bot has no character: " + name;
            return false;
        }

        BotLiveStatus st;
        if (!_bridge->GetLiveStatus(rec.guid, st) || !st.online || !st.controlled)
        {
            outErr = "bot must be online and bot-controlled first: " + name;
            return false;
        }

        // Force the factory gear + talent spec at the bot's current level, bypassing
        // the L<=5 starter-kit gate (used to make a manually-leveled bot test-ready).
        bool const ok = _bridge->EnsureStarterGear(rec.guid);
        _bridge->AutoSpecTalents(rec.guid);
        rec.starterKitDone = true;          // don't let the low-level path re-gear later
        rec.lastSpeccedLevel = st.level;
        sIdleBotLog->Write(name, "EVENT", Acore::StringFormat("force-geared at level {}", st.level));
        if (!ok)
        {
            outErr = "EnsureStarterGear failed (see server log)";
            return false;
        }
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
        BotRecord& rec = bit->second;
        rec.guideId = guideId;
        rec.currentStepIndex = 0;
        rec.stepState = "idle";
        rec.deathCountStep = 0;
        ResetObjectStepState(rec);

        auto const& guide = _guides[guideId];
        uint32_t const skipped = FastForwardRewardedSteps(rec, guide, "set-guide");

        PersistProgress(rec);
        if (skipped > 0)
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': guide '{}' assigned, skipped {} already-done steps.",
                name, guideId, skipped);
        sIdleBotLog->Write(name, "GUIDE", "assigned guide '" + guideId + "'");
        LOG_INFO("module.idlebot", "[IdleBot] bot '{}': guide set to '{}' at step {}.",
            name, guideId, rec.currentStepIndex + 1);
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
        BotRecord& rec = bit->second;
        rec.guideId.clear();
        rec.currentStepIndex = 0;
        rec.stepState = "idle";
        ResetObjectStepState(rec);
        PersistProgress(rec);
        sIdleBotLog->Write(name, "GUIDE", "guide cleared");
        return true;
    }

    std::string IdleBotManager::GuideCurrent(const std::string& rawName) const
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end())
            return "No such bot: " + name;

        const BotRecord& rec = it->second;
        if (rec.guideId.empty())
            return "Bot " + name + ": no guide assigned.";

        auto git = _guides.find(rec.guideId);
        std::size_t total = (git != _guides.end()) ? git->second.steps.size() : 0;
        std::string out = Acore::StringFormat("Bot {}: guide '{}' step {}/{} [{}]",
            name, rec.guideId, rec.currentStepIndex + 1, total, rec.stepState);
        if (git != _guides.end() && rec.currentStepIndex < total)
            out += Acore::StringFormat("\n  current: {}", git->second.steps[rec.currentStepIndex].name);
        return out;
    }

    bool IdleBotManager::GuideReset(const std::string& rawName, std::string& outErr)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end())
        {
            outErr = "no such bot";
            return false;
        }
        BotRecord& rec = it->second;
        if (rec.guideId.empty())
        {
            outErr = "bot has no guide to reset";
            return false;
        }
        rec.currentStepIndex = 0;
        rec.stepState = "idle";
        rec.deathCountStep = 0;
        ResetObjectStepState(rec);
        PersistProgress(rec);
        sIdleBotLog->Write(name, "GUIDE", "guide reset to step 1");
        return true;
    }

    bool IdleBotManager::SetGuideStep(const std::string& rawName, uint32_t index, std::string& outErr)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end())
        {
            outErr = "no such bot";
            return false;
        }
        BotRecord& rec = it->second;
        if (rec.guideId.empty())
        {
            outErr = "bot has no guide";
            return false;
        }
        auto git = _guides.find(rec.guideId);
        if (git == _guides.end() || index >= git->second.steps.size())
        {
            outErr = "step index out of range";
            return false;
        }
        rec.currentStepIndex = index;
        rec.stepState = "idle";
        rec.deathCountStep = 0;
        ResetObjectStepState(rec);
        PersistProgress(rec);
        sIdleBotLog->Write(name, "GUIDE", Acore::StringFormat("jumped to step {}", index + 1));
        return true;
    }

    std::string IdleBotManager::SummaryOf(const std::string& rawName) const
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end())
            return "No such bot: " + name;

        const BotRecord& rec = it->second;
        std::string out = "=== " + name + " ===";
        out += rec.paused ? " [paused]" : (rec.active ? " [active]" : " [inactive]");

        if (_bridge)
        {
            BotGuid guid = rec.guid ? rec.guid : _bridge->GetBotGuid(name);
            BotLiveStatus st;
            if (guid && _bridge->GetLiveStatus(guid, st) && st.online)
            {
                uint32_t xp = 0, xpNext = 0;
                _bridge->GetXp(guid, xp, xpNext);
                out += Acore::StringFormat("\n  level {} ({}/{} xp){}",
                    st.level, xp, xpNext, st.controlled ? "" : "  [NOT bot-controlled]");
                out += Acore::StringFormat("\n  hp {}/{}  mana {}/{}",
                    st.health, st.maxHealth, st.mana, st.maxMana);
                if (st.pos.valid)
                    out += Acore::StringFormat("\n  location: map {} ({:.0f}, {:.0f}, {:.0f})",
                        st.pos.mapId, st.pos.x, st.pos.y, st.pos.z);
                InventoryStatus inv = _bridge->GetInventoryStatus(guid);
                if (inv.valid)
                    out += Acore::StringFormat("\n  bags: {}/{} free   durability: {}%",
                        inv.freeSlots, inv.totalSlots, inv.lowestDurabilityPct);
                out += Acore::StringFormat("\n  active quests: {}", st.questCount);
            }
            else
            {
                out += "\n  offline";
            }
        }

        if (rec.guideId.empty())
        {
            out += "\n  guide: (none)";
        }
        else
        {
            auto git = _guides.find(rec.guideId);
            std::size_t total = (git != _guides.end()) ? git->second.steps.size() : 0;
            out += Acore::StringFormat("\n  guide: {} — step {}/{} [{}]",
                rec.guideId, rec.currentStepIndex + 1, total, rec.stepState);
            if (git != _guides.end() && rec.currentStepIndex < total)
                out += Acore::StringFormat("\n  objective: {}", git->second.steps[rec.currentStepIndex].name);
        }
        out += Acore::StringFormat("\n  deaths: {} total ({} on current step)",
            rec.deathCountTotal, rec.deathCountStep);
        return out;
    }

    void IdleBotManager::RegisterGuide(Guide g)
    {
        std::string const id = g.id;
        auto it = _guides.find(id);
        if (it == _guides.end())
        {
            _guides.emplace(id, std::move(g));
            return;
        }

        it->second = std::move(g);
        LOG_INFO("module.idlebot", "[IdleBot] guide '{}' overridden by later registration.", id);
    }

    void IdleBotManager::LoadConfiguredGuides()
    {
        std::string const configuredDirectory =
            sConfigMgr->GetOption<std::string>("IdleBot.GuideDirectory", "./modules/mod-idlebot/data/guides");
        std::string resolvedFrom;
        std::string const guideDirectory = ResolveGuideDirectory(configuredDirectory, resolvedFrom);
        _guideDirectory = guideDirectory;

        if (guideDirectory != configuredDirectory)
        {
            LOG_INFO("module.idlebot",
                "[IdleBot] guide directory '{}' not found, using fallback '{}'.",
                configuredDirectory,
                guideDirectory);
        }

        IdleBotGuideLoader loader;
        size_t const loaded = loader.LoadDirectory(guideDirectory);
        if (loaded == 0)
        {
            LOG_INFO("module.idlebot", "[IdleBot] no file guides loaded from '{}'.", guideDirectory);
            return;
        }

        for (std::string const& guideId : loader.ListIds())
        {
            std::optional<Guide> guide = loader.Get(guideId);
            if (guide.has_value())
                RegisterGuide(std::move(*guide));
        }

        LOG_INFO("module.idlebot", "[IdleBot] loaded {} file guide(s) from '{}'.", loaded, guideDirectory);
    }

    IdleBotManager::GuideReloadResult IdleBotManager::ReloadAllGuides()
    {
        GuideReloadResult result;
        if (_guideDirectory.empty())
        {
            result.errorMsg = "guide directory not set (server not initialized?)";
            return result;
        }

        // Parse new guide data into a temporary loader — don't touch _guides yet.
        IdleBotGuideLoader loader;
        size_t const loaded = loader.LoadDirectory(_guideDirectory);
        result.guidesLoaded = static_cast<uint32_t>(loaded);

        // Validate every guide before committing the swap.
        for (std::string const& id : loader.ListIds())
        {
            std::optional<Guide> g = loader.Get(id);
            if (!g.has_value())
                continue;
            std::string err;
            if (!g->Valid(err))
            {
                result.validationErrors++;
                if (result.errorFile.empty())
                {
                    result.errorFile = id;
                    result.errorMsg  = err;
                }
            }
        }

        if (result.validationErrors > 0)
        {
            result.success = false;
            LOG_WARN("module.idlebot",
                "[IdleBot] guide reload aborted — {} validation error(s). Old guides remain active.",
                result.validationErrors);
            return result;
        }

        // Record which bots are affected before the swap.
        for (auto& [name, rec] : _bots)
        {
            if (!rec.guideId.empty() && loader.Get(rec.guideId).has_value())
                result.affectedBots.push_back(rec.name);
        }

        // Atomic swap: replace registry.
        _guides.clear();
        for (std::string const& id : loader.ListIds())
        {
            std::optional<Guide> g = loader.Get(id);
            if (g.has_value())
                RegisterGuide(std::move(*g));
        }

        // Rebind every active bot to the new guide data.
        for (auto& [name, rec] : _bots)
            RebindBotGuide(rec);

        ++_guideReloadCount;
        result.success = true;

        LOG_INFO("module.idlebot",
            "[IdleBot] guide reload #{}: {} guide(s) loaded, {} bot(s) affected.",
            _guideReloadCount, result.guidesLoaded, result.affectedBots.size());

        for (std::string const& botName : result.affectedBots)
        {
            auto it = _bots.find(NormalizeName(botName));
            if (it != _bots.end())
                EmitEvent(it->second, "GUIDE_RELOADED", "guide registry reloaded");
        }

        return result;
    }

    IdleBotManager::GuideReloadResult IdleBotManager::ReloadGuide(std::string const& pathOrId)
    {
        GuideReloadResult result;
        if (_guideDirectory.empty())
        {
            result.errorMsg = "guide directory not set (server not initialized?)";
            return result;
        }

        // Relative paths are joined with the guide directory.
        std::string fullPath = pathOrId;
        if (!pathOrId.empty() && pathOrId[0] != '/')
            fullPath = _guideDirectory + "/" + pathOrId;

        IdleBotGuideLoader loader;
        size_t const loaded = loader.LoadFile(fullPath);
        if (loaded == 0)
        {
            result.errorFile = fullPath;
            result.errorMsg  = "failed to load file (check path and YAML syntax)";
            result.success   = false;
            return result;
        }
        result.guidesLoaded = static_cast<uint32_t>(loaded);

        for (std::string const& id : loader.ListIds())
        {
            std::optional<Guide> g = loader.Get(id);
            if (!g.has_value())
                continue;
            std::string err;
            if (!g->Valid(err))
            {
                result.validationErrors++;
                if (result.errorFile.empty())
                {
                    result.errorFile = fullPath;
                    result.errorMsg  = err;
                }
            }
        }

        if (result.validationErrors > 0)
        {
            result.success = false;
            LOG_WARN("module.idlebot",
                "[IdleBot] guide reload aborted for '{}' — {} validation error(s). Old guide remains active.",
                pathOrId, result.validationErrors);
            return result;
        }

        // Merge updated guide(s) into the live registry.
        for (std::string const& id : loader.ListIds())
        {
            for (auto& [name, rec] : _bots)
            {
                if (rec.guideId == id &&
                    std::find(result.affectedBots.begin(), result.affectedBots.end(), rec.name) == result.affectedBots.end())
                    result.affectedBots.push_back(rec.name);
            }
            std::optional<Guide> g = loader.Get(id);
            if (g.has_value())
                RegisterGuide(std::move(*g));
        }

        for (std::string const& botName : result.affectedBots)
        {
            auto it = _bots.find(NormalizeName(botName));
            if (it != _bots.end())
            {
                RebindBotGuide(it->second);
                EmitEvent(it->second, "GUIDE_RELOADED",
                    Acore::StringFormat("guide reloaded from {}", pathOrId));
            }
        }

        ++_guideReloadCount;
        result.success = true;

        LOG_INFO("module.idlebot",
            "[IdleBot] guide '{}' reloaded: {} guide(s) updated, {} bot(s) rebound.",
            pathOrId, result.guidesLoaded, result.affectedBots.size());

        return result;
    }

    std::string IdleBotManager::ValidateGuideFile(std::string const& path)
    {
        std::string fullPath = path;
        if (!path.empty() && path[0] != '/' && !_guideDirectory.empty())
            fullPath = _guideDirectory + "/" + path;

        IdleBotGuideLoader loader;
        std::string err;
        if (!loader.ValidateFile(fullPath, err))
            return err;
        return {};
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
        // NOTE: Quest 3902 (Scavenging Deathknell) uses gameobject interaction
        //   (InteractGameobject step, GO entry 164662) — completed via the loot path.
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
            // `mobs` = the creature entries that satisfy this objective (kill-credit
            // NPCs or the creatures that drop the required items). The executor homes
            // onto the nearest of these so it grinds the right mobs. Entries +
            // spawn-centre coords verified from acore_world on zoidberg.
            auto ki = [](std::string id, std::string name, uint32_t quest, std::vector<uint32_t> mobs, uint32_t map, float x, float y, float z, float r = 60.f, std::string completion = std::string()) {
                GuideStep s;
                s.id = std::move(id);
                s.name = std::move(name);
                s.type = StepType::KillMobs;
                s.questId = quest;
                s.creatureIds = std::move(mobs);
                s.coords = { map, x, y, z, r, false };
                s.completionCondition = std::move(completion);
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
            auto ig = [](std::string id, std::string name, uint32_t quest, uint32_t go, uint32_t item, uint32_t map, float x, float y, float z, float r, std::string completion = std::string()) {
                GuideStep s;
                s.id = std::move(id);
                s.name = std::move(name);
                s.type = StepType::InteractGameobject;
                s.questId = quest;
                s.gameobjectId = go;
                s.itemId = item;
                s.coords = { map, x, y, z, r, false };
                s.completionCondition = std::move(completion);
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
                364, { 1501, 1502 }, 0, 1930.f, 1553.f, 85.f, 80.f));
            g.steps.push_back(mv("q364_return_sarvis", "return to Executor Sarvis",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(tq("q364_turnin", "turn in The Mindless Ones (364)",
                364, 1569, 0, 1843.32f, 1639.9f, 97.8f));

            // Q376: Rattling the Rattlecages (L2) → Q3901 prereq, Elreth gives, 6 bat wings + 6 paws
            g.steps.push_back(mv("q376_go_elreth", "go to Novice Elreth for Rattling the Rattlecages",
                0, 1847.73f, 1638.65f, 97.0f, 6.f));
            g.steps.push_back(aq("q376_accept", "accept Rattling the Rattlecages (376)",
                376, 1661, 0, 1847.73f, 1638.65f, 97.0f));
            // Duskbats (1512) ~(1857,1616); Young Scavengers (1508) ~(1952,1617).
            // The bot is a mage (ranged) so it CAN kill the flying duskbats; centre
            // between the two clusters and let it home onto whichever is nearest.
            g.steps.push_back(mv("q376_go_bats", "go to the duskbat/scavenger area",
                0, 1900.f, 1616.f, 96.f, 60.f));
            g.steps.push_back(ki("q376_kill", "kill Duskbats and Young Scavengers (q376)",
                376, { 1512, 1508 }, 0, 1900.f, 1616.f, 96.f, 90.f));
            g.steps.push_back(mv("q376_return_elreth", "return to Novice Elreth",
                0, 1847.73f, 1638.65f, 97.0f, 6.f));
            g.steps.push_back(tq("q376_turnin", "turn in Rattling the Rattlecages (376)",
                376, 1661, 0, 1847.73f, 1638.65f, 97.0f));

            // Q3901: Rattling the Rattlecages (L3) — Sarvis gives, 8 Rattlecage Skeletons
            g.steps.push_back(mv("q3901_go_sarvis", "go to Shadow Priest Sarvis for Rattling the Rattlecages",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(aq("q3901_accept", "accept Rattling the Rattlecages (3901)",
                3901, 1569, 0, 1843.32f, 1639.9f, 97.8f));
            g.steps.push_back(mv("q3901_go_skeletons", "go to Rattlecage Skeleton area",
                0, 1979.f, 1542.f, 81.f, 60.f));
            g.steps.push_back(ki("q3901_kill", "kill Rattlecage Skeletons (q3901)",
                3901, { 1890 }, 0, 1979.f, 1542.f, 81.f, 80.f));
            g.steps.push_back(mv("q3901_return_sarvis", "return to Executor Sarvis",
                0, 1843.32f, 1639.9f, 97.8f, 6.f));
            g.steps.push_back(tq("q3901_turnin", "turn in Rattling the Rattlecages (3901)",
                3901, 1569, 0, 1843.32f, 1639.9f, 97.8f));

            // Q3902: Scavenging Deathknell (L3) — Saltain gives, 6 Scavenged Goods looted
            // from "Equipment Boxes" gameobjects (GO entry 164662, loot id 10984 → item
            // 11127). Verified from acore_world: gameobject_template 164662 (type 3 chest),
            // gameobject_loot_template (10984,11127), quest_template 3902 ReqItem 11127 x6.
            // Box spawns cluster around (1900,1545,88) in Deathknell; search radius 120.
            g.steps.push_back(mv("q3902_go_saltain", "go to Deathguard Saltain for Scavenging Deathknell",
                0, 1861.17f, 1605.02f, 95.0f, 6.f));
            g.steps.push_back(aq("q3902_accept", "accept Scavenging Deathknell (3902)",
                3902, 1740, 0, 1861.17f, 1605.02f, 95.0f));
            g.steps.push_back(mv("q3902_go_boxes", "go to the Deathknell equipment boxes",
                0, 1900.f, 1545.f, 88.f, 30.f));
            g.steps.push_back(ig("q3902_scavenge", "scavenge Equipment Boxes for Scavenged Goods (q3902)",
                3902, 164662, 11127, 0, 1900.f, 1545.f, 88.f, 120.f, "quest_objective_complete:3902/1"));
            g.steps.push_back(mv("q3902_return_saltain", "return to Deathguard Saltain",
                0, 1861.17f, 1605.02f, 95.0f, 6.f));
            g.steps.push_back(tq("q3902_turnin", "turn in Scavenging Deathknell (3902)",
                3902, 1740, 0, 1861.17f, 1605.02f, 95.0f));

            // Q380: Night Web's Hollow (L4) — Arren gives, 8 young spiders + 5 night spiders
            g.steps.push_back(mv("q380_go_arren", "go to Executor Arren for Night Web's Hollow",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q380_accept", "accept Night Web's Hollow (380)",
                380, 1570, 0, 1848.82f, 1580.47f, 94.7f));
            g.steps.push_back(mv("q380_go_spiders", "go to Night Web spider area",
                0, 2060.f, 1800.f, 90.f, 80.f));
            g.steps.push_back(ki("q380_kill_young", "kill Young Night Web Spiders (q380/1)",
                380, { 1504 }, 0, 2101.69f, 1745.69f, 88.54f, 80.f, "quest_objective_complete:380/1"));
            g.steps.push_back(mv("q380_go_cave", "go inside the Night Web cave",
                0, 2039.29f, 1915.53f, 102.66f, 30.f));
            g.steps.push_back(ki("q380_kill_night", "kill Night Web Spiders (q380/2)",
                380, { 1505 }, 0, 2039.29f, 1915.53f, 102.66f, 60.f, "quest_objective_complete:380/2"));
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
                381, { 1506, 1507 }, 0, 1808.f, 1339.f, 90.f, 90.f));
            g.steps.push_back(mv("q381_return_arren", "return to Executor Arren",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(tq("q381_turnin", "turn in The Scarlet Crusade (381)",
                381, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // Q382: The Red Messenger (L5) — Arren gives, kill Meven Korgal (1667) for docs
            g.steps.push_back(mv("q382_go_arren", "go to Executor Arren for The Red Messenger",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q382_accept", "accept The Red Messenger (382)",
                382, 1570, 0, 1848.82f, 1580.47f, 94.7f));
            g.steps.push_back(mv("q382_go_korgal", "go to Meven Korgal",
                0, 1772.f, 1381.f, 91.f, 15.f));
            g.steps.push_back(ki("q382_kill", "kill Meven Korgal for Scarlet Crusade Documents (q382)",
                382, { 1667 }, 0, 1773.f, 1381.f, 91.f, 25.f, "quest_objective_complete:382/1"));
            g.steps.push_back(mv("q382_return_arren", "return to Executor Arren",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(tq("q382_turnin", "turn in The Red Messenger (382)",
                382, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // Q383: Vital Intelligence (L5) — Arren gives, run to Zygand in Brill
            g.steps.push_back(mv("q383_go_arren", "go to Executor Arren for Vital Intelligence",
                0, 1848.82f, 1580.47f, 94.7f, 6.f));
            g.steps.push_back(aq("q383_accept", "accept Vital Intelligence (383)",
                383, 1570, 0, 1848.82f, 1580.47f, 94.7f));

            // ---- Travel to Brill ----
            g.steps.push_back(mv("brill_travel", "travel road to Brill",
                0, 2278.08f, 295.587f, 35.3f, 15.f));

            // Q383 turn-in at Zygand
            g.steps.push_back(tq("q383_turnin", "turn in Vital Intelligence (383) to Zygand",
                383, 1515, 0, 2278.08f, 295.587f, 35.3f));

            // ---- Brill quests ----

            // Q367: A New Plague (L6) — 5 Darkhound Blood from nearby darkhounds
            g.steps.push_back(mv("q367_go_johaan", "go to Apothecary Johaan for A New Plague",
                0, 2259.04f, 347.048f, 36.1f, 6.f));
            g.steps.push_back(aq("q367_accept", "accept A New Plague (367)",
                367, 1518, 0, 2259.04f, 347.048f, 36.1f));

            // Q404: A Putrid Task (L4) — 7 Putrid Claws from Rotting Dead
            g.steps.push_back(mv("q404_go_dillinger", "go to Deathguard Dillinger",
                0, 2287.66f, 403.372f, 34.0f, 6.f));
            g.steps.push_back(aq("q404_accept", "accept A Putrid Task (404)",
                404, 1496, 0, 2287.66f, 403.372f, 34.0f));

            // Q374: Proof of Demise (L5) — 10 Scarlet Insignia Rings from nearby Scarlet mobs
            g.steps.push_back(mv("q374_go_burgess", "go to Deathguard Burgess for Proof of Demise",
                0, 2270.7f, 279.998f, 35.3f, 6.f));
            g.steps.push_back(aq("q374_accept", "accept Proof of Demise (374)",
                374, 1652, 0, 2270.7f, 279.998f, 35.3f));

            // Q427: Zygand (L8) — kill 10 Scarlet Warriors; pick up before grinding them
            g.steps.push_back(mv("q427_go_zygand", "go to Executor Zygand for At War With The Scarlet Crusade",
                0, 2278.08f, 295.587f, 35.3f, 6.f));
            g.steps.push_back(aq("q427_accept", "accept At War With The Scarlet Crusade (427)",
                427, 1515, 0, 2278.08f, 295.587f, 35.3f));

            // Kill Rotting Dead + Darkhounds in one area sweep
            g.steps.push_back(mv("q404_go_rotting", "go to Rotting Dead area for Putrid Claws",
                0, 2241.f, 621.f, 34.f, 80.f));
            g.steps.push_back(ki("q404_kill", "kill Rotting Dead for Putrid Claws (q404)",
                404, { 1525, 1526 }, 0, 2241.f, 621.f, 34.f, 110.f));

            g.steps.push_back(mv("q367_go_darkhounds", "go to Rot Hide Darkhound area",
                0, 2282.f, 448.f, 42.f, 140.f));
            g.steps.push_back(ki("q367_kill", "kill nearby Darkhounds for blood (q367)",
                367, { 1547, 1548, 1549 }, 0, 2282.f, 448.f, 42.f, 180.f, "quest_objective_complete:367/1"));

            // Kill Scarlet Warriors for q374 + q427 simultaneously
            g.steps.push_back(mv("q374_go_scarlets", "go to Scarlet Warrior area",
                0, 2391.f, 1564.f, 40.f, 80.f));
            g.steps.push_back(ki("q374_kill", "kill Scarlet Warriors for insignia rings (q374)",
                374, { 1535 }, 0, 2391.f, 1565.f, 40.f, 100.f, "quest_objective_complete:374/1"));
            g.steps.push_back(ki("q427_kill", "kill Scarlet Warriors for kill count (q427)",
                427, { 1535 }, 0, 2391.f, 1565.f, 40.f, 100.f, "quest_objective_complete:427/1"));

            // Turn in Brill quests
            g.steps.push_back(mv("q404_return_dillinger", "return to Deathguard Dillinger",
                0, 2287.66f, 403.372f, 34.0f, 6.f));
            g.steps.push_back(tq("q404_turnin", "turn in A Putrid Task (404)",
                404, 1496, 0, 2287.66f, 403.372f, 34.0f));

            g.steps.push_back(mv("q367_return_johaan", "return to Doctor Johaan",
                0, 2259.04f, 347.048f, 36.1f, 6.f));
            g.steps.push_back(tq("q367_turnin", "turn in A New Plague (367)",
                367, 1518, 0, 2259.04f, 347.048f, 36.1f));

            g.steps.push_back(mv("q374_return_burgess", "return to Deathguard Burgess",
                0, 2270.7f, 279.998f, 35.3f, 6.f));
            g.steps.push_back(tq("q374_turnin", "turn in Proof of Demise (374)",
                374, 1652, 0, 2270.7f, 279.998f, 35.3f));

            g.steps.push_back(mv("q427_return_zygand", "return to Deathguard Zygand",
                0, 2278.08f, 295.587f, 35.3f, 6.f));
            g.steps.push_back(tq("q427_turnin", "turn in At War With The Scarlet Crusade (427)",
                427, 1515, 0, 2278.08f, 295.587f, 35.3f));

            // Q370: At War With The Scarlet Crusade (2) — Perrine + Missionaries + Zealots
            // Giver and turn-in are both Executor Zygand (1515) at (2278,296);
            // Magistrate Sevren (1499) nearby does NOT start this quest.
            g.steps.push_back(mv("q370_go_zygand", "go to Executor Zygand for At War With The Scarlet Crusade",
                0, 2278.08f, 295.587f, 35.3f, 6.f));
            g.steps.push_back(aq("q370_accept", "accept At War With The Scarlet Crusade (370)",
                370, 1515, 0, 2278.08f, 295.587f, 35.3f));
            g.steps.push_back(mv("q370_go_missionaries", "go to Scarlet Missionary area",
                0, 1824.33f, 812.9f, 37.38f, 60.f));
            g.steps.push_back(ki("q370_kill_missionaries", "kill Scarlet Missionaries (q370/3)",
                370, { 1536 }, 0, 1824.33f, 812.9f, 37.38f, 80.f, "quest_objective_complete:370/3"));
            g.steps.push_back(mv("q370_go_perrine", "go to Captain Perrine",
                0, 1795.12f, 722.66f, 49.09f, 20.f));
            g.steps.push_back(ki("q370_kill_perrine", "kill Captain Perrine (q370/1)",
                370, { 1662 }, 0, 1795.12f, 722.66f, 49.09f, 25.f, "quest_objective_complete:370/1"));
            // Scarlet Zealots (1537) cluster ~(2156,-529) — NOT (2154,-192).
            g.steps.push_back(mv("q370_go_zealots", "go to Scarlet Zealot area",
                0, 2156.2f, -528.6f, 80.3f, 60.f));
            g.steps.push_back(ki("q370_kill_zealots", "kill Scarlet Zealots (q370/2)",
                370, { 1537 }, 0, 2156.2f, -528.6f, 80.3f, 180.f, "quest_objective_complete:370/2"));
            g.steps.push_back(mv("q370_return_zygand", "return to Executor Zygand",
                0, 2278.08f, 295.587f, 35.3f, 6.f));
            g.steps.push_back(tq("q370_turnin", "turn in At War With The Scarlet Crusade (370)",
                370, 1515, 0, 2278.08f, 295.587f, 35.3f));

            RegisterGuide(std::move(g));
        }

        LOG_INFO("module.idlebot", "[IdleBot] registered {} builtin guide(s).", _guides.size());
    }

    void IdleBotManager::WriteLiveState(BotRecord& rec)
    {
        if (!_bridge)
            return;

        // Position and live stats from the bridge.
        BotPosition const pos    = _bridge->GetPosition(rec.guid);
        bool const isDead        = _bridge->IsDead(rec.guid);
        bool const isGhost       = _bridge->IsGhost(rec.guid);
        uint32_t const level     = _bridge->GetLevel(rec.guid);
        uint8_t const classId    = _bridge->GetClass(rec.guid);
        uint8_t const raceId     = _bridge->GetRace(rec.guid);

        BotLiveStatus live;
        _bridge->GetLiveStatus(rec.guid, live);
        InventoryStatus const inv = _bridge->GetInventoryStatus(rec.guid);

        CombatContext cc;
        _bridge->GetCombatContext(rec.guid, cc);

        // Current guide step metadata.
        uint32_t stepTotal = 0;
        std::string stepName;
        uint32_t questId = 0;
        std::string objectiveText;
        if (!rec.guideId.empty())
        {
            auto git = _guides.find(rec.guideId);
            if (git != _guides.end())
            {
                stepTotal = static_cast<uint32_t>(git->second.steps.size());
                if (rec.currentStepIndex < stepTotal)
                {
                    GuideStep const& step = git->second.steps[rec.currentStepIndex];
                    stepName = step.name;
                    if (step.questId.has_value())
                        questId = *step.questId;
                    objectiveText = step.completionCondition;
                }
            }
        }

        // Target role string from last-engaged tracking.
        char const* targetRole = "";
        if (rec.lastEngagedWasPathBlocker)
            targetRole = "path_blocker";
        else if (rec.lastEngagedWasObjective)
            targetRole = "objective_target";
        else if (rec.lastEngagedGuid != 0)
            targetRole = "defensive_add";

        uint32_t durPct   = inv.lowestDurabilityPct;
        uint32_t money    = rec.guid ? _bridge->GetMoney(rec.guid) : 0;

        // Derive bag totals: inventory returns free + total; used = total - free.
        uint32_t bagUsed  = inv.valid ? (inv.totalSlots - inv.freeSlots) : 0;
        uint32_t bagTotal = inv.valid ? inv.totalSlots : 0;
        uint32_t charGuid = rec.guid ? static_cast<uint32_t>(rec.guid & 0xFFFFFFFF) : 0;

        // Escape strings that may contain single quotes (quest names, step names, etc.)
        CharacterDatabase.EscapeString(stepName);
        CharacterDatabase.EscapeString(objectiveText);
        std::string targetNameEsc = cc.currentTargetName;
        CharacterDatabase.EscapeString(targetNameEsc);
        std::string blockedReasonEsc = rec.blockedReason;
        CharacterDatabase.EscapeString(blockedReasonEsc);
        std::string blockedSinceEsc = rec.blockedSince;
        CharacterDatabase.EscapeString(blockedSinceEsc);
        std::string const blockedReasonClause =
            blockedReasonEsc.empty() ? std::string("NULL") : ("'" + blockedReasonEsc + "'");
        std::string const blockedSinceClause =
            blockedSinceEsc.empty() ? std::string("NULL") : ("'" + blockedSinceEsc + "'");
        std::string const lastFailureClause =
            rec.lastFailureCode.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.lastFailureCode) + "'");

        // State string for the dashboard.
        char const* stateStr = rec.stepState.c_str();
        if (rec.stepState == "blocked" || !rec.blockedReason.empty())
            stateStr = "blocked";
        else if (rec.paused)
            stateStr = "paused";
        else if (isGhost || isDead)
            stateStr = "dead";
        else if (rec.deathPhase != DeathPhase::Alive)
            stateStr = "recover";

        CharacterDatabase.Execute(
            "INSERT INTO idlebot_live_state "
            "(bot_name, character_guid, class_id, race_id, level, "
            " map_id, x, y, z, orientation, "
            " hp, hp_max, mana, mana_max, hp_pct, mana_pct, "
            " alive, is_ghost, in_combat, state, "
            " guide_id, soak_run_id, bot_session_id, reset_id, step_index, step_total, step_name, blocked_reason, blocked_since, last_failure_code, requires_user_action, "
            " quest_id, objective_text, "
            " target_entry, target_name, target_level, target_distance, target_role, "
            " death_count_total, death_count_current_step, "
            " money, bag_used, bag_total, durability_pct, updated_at) "
            "VALUES ('{}', {}, {}, {}, {}, "
            "        {}, {:.2f}, {:.2f}, {:.2f}, {:.4f}, "
            "        {}, {}, {}, {}, {:.1f}, {:.1f}, "
            "        {}, {}, {}, '{}', "
            "        '{}', '{}', '{}', {}, {}, {}, '{}', {}, {}, {}, "
            "        {}, {}, '{}', "
            "        {}, '{}', {}, {}, '{}', "
            "        {}, {}, "
            "        {}, {}, {}, {}, UTC_TIMESTAMP()) "
            "ON DUPLICATE KEY UPDATE "
            "  character_guid=VALUES(character_guid), class_id=VALUES(class_id), "
            "  race_id=VALUES(race_id), level=VALUES(level), "
            "  map_id=VALUES(map_id), x=VALUES(x), y=VALUES(y), z=VALUES(z), "
            "  orientation=VALUES(orientation), "
            "  hp=VALUES(hp), hp_max=VALUES(hp_max), "
            "  mana=VALUES(mana), mana_max=VALUES(mana_max), "
            "  hp_pct=VALUES(hp_pct), mana_pct=VALUES(mana_pct), "
            "  alive=VALUES(alive), is_ghost=VALUES(is_ghost), "
            "  in_combat=VALUES(in_combat), state=VALUES(state), "
            "  guide_id=VALUES(guide_id), soak_run_id=VALUES(soak_run_id), bot_session_id=VALUES(bot_session_id), "
            "  reset_id=VALUES(reset_id), step_index=VALUES(step_index), "
            "  step_total=VALUES(step_total), step_name=VALUES(step_name), "
            "  blocked_reason=VALUES(blocked_reason), blocked_since=VALUES(blocked_since), "
            "  last_failure_code=VALUES(last_failure_code), requires_user_action=VALUES(requires_user_action), "
            "  quest_id=VALUES(quest_id), objective_text=VALUES(objective_text), "
            "  target_entry=VALUES(target_entry), target_name=VALUES(target_name), "
            "  target_level=VALUES(target_level), target_distance=VALUES(target_distance), "
            "  target_role=VALUES(target_role), "
            "  death_count_total=VALUES(death_count_total), "
            "  death_count_current_step=VALUES(death_count_current_step), "
            "  money=VALUES(money), bag_used=VALUES(bag_used), bag_total=VALUES(bag_total), "
            "  durability_pct=VALUES(durability_pct), updated_at=UTC_TIMESTAMP()",
            rec.name,
            charGuid,
            classId, raceId, level,
            pos.valid ? pos.mapId : 0u,
            pos.valid ? pos.x : 0.f,
            pos.valid ? pos.y : 0.f,
            pos.valid ? pos.z : 0.f,
            pos.valid ? pos.o : 0.f,
            live.health, live.maxHealth,
            live.mana, live.maxMana,
            cc.valid ? cc.hpPct : 100.f,
            cc.valid ? cc.manaPct : 100.f,
            (!isDead && !isGhost) ? 1 : 0,
            isGhost ? 1 : 0,
            cc.inCombat ? 1 : 0,
            stateStr,
            rec.guideId,
            rec.soakRunId,
            rec.botSessionId,
            rec.resetId,
            rec.currentStepIndex, stepTotal,
            stepName,
            blockedReasonClause,
            blockedSinceClause,
            lastFailureClause,
            rec.requiresUserAction ? 1 : 0,
            questId,
            objectiveText,
            cc.currentTargetEntry,
            targetNameEsc,
            0,   // target_level: not in CombatContext; use 0 for now
            cc.currentTargetDistance,
            targetRole,
            rec.deathCountTotal, rec.deathCountStep,
            money,
            bagUsed, bagTotal,
            durPct);

    }
}
