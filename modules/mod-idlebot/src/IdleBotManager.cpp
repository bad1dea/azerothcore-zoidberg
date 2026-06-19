#include "IdleBotManager.h"
#include "IdleBotLog.h"
#include "IdleBotZoneRoute.h"
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
    }

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

        // Death handling (Priority 2).
        _deathEnabled            = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.Enabled", true);
        _allowDirectResurrect    = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.AllowDirectResurrect", true);
        _allowGraveyardResurrect = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.AllowGraveyardResurrect", true);
        _maxCorpseRunAttempts    = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.MaxCorpseRunAttempts", 3);
        _maxDeathsPerStep        = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.MaxDeathsPerStep", 3);
        _pauseAfterDeathLoop     = sConfigMgr->GetOption<bool>("IdleBot.DeathHandling.PauseAfterDeathLoop", true);
        _ghostStallTicks         = sConfigMgr->GetOption<uint32_t>("IdleBot.DeathHandling.GhostStallTicks", 8);

        // Inventory / town maintenance (Priority 5).
        _townMaintenanceEnabled  = sConfigMgr->GetOption<bool>("IdleBot.TownMaintenance.Enabled", true);
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

        // Adaptive combat (smart engagement modes).
        _maxPull                 = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.MaxPull", 3);
        _lowHpPct                = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.LowHpPct", 35);
        _lowManaPct              = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.LowManaPct", 20);
        _aoeThreshold            = sConfigMgr->GetOption<uint32_t>("IdleBot.Combat.AoeThreshold", 3);
        _rangedKite              = sConfigMgr->GetOption<bool>("IdleBot.Combat.RangedKite", true);
        _autoGear                = sConfigMgr->GetOption<bool>("IdleBot.AutoGear", false);

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
        if (!_bridge)
            return;

        if (!rec.guid)
            rec.guid = _bridge->GetBotGuid(rec.name);

        BotLiveStatus live;
        bool const liveKnown = rec.guid && _bridge->GetLiveStatus(rec.guid, live);
        if (!liveKnown || !live.online)
        {
            rec.controlWaitTicks = 0;
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
            if (rec.controlWaitTicks == 0)
            {
                LOG_WARN("module.idlebot", "[IdleBot] bot '{}': online but not under playerbot control yet; waiting.", rec.name);
                rec.controlWaitTicks = 30;
            }
            else
                --rec.controlWaitTicks;
            return;
        }

        rec.loginRetryTicks = 0;
        rec.controlWaitTicks = 0;

        // One-time per-session setup (ensure looting strategy is on).
        EnsureStrategies(rec);

        // Death recovery overrides everything (Pitfall D). HandleDeath returns true
        // while the bot is dead/recovering — skip the rest of the tick so we never
        // issue guide movement/combat actions that fight the dead-state AI.
        if (HandleDeath(rec))
            return;

        // Emit IdleRPG events from polled deltas (level/quest/loot/inventory).
        PollDeltas(rec);

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
            return;

        auto git = _guides.find(rec.guideId);
        if (git == _guides.end())
        {
            LOG_WARN("module.idlebot", "[IdleBot] bot '{}': guide '{}' not found — clearing.", rec.name, rec.guideId);
            rec.guideId.clear();
            rec.currentStepIndex = 0;
            ResetObjectStepState(rec);
            PersistProgress(rec);
            return;
        }

        const Guide& guide = git->second;
        if (rec.currentStepIndex >= guide.steps.size())
        {
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': guide '{}' complete!", rec.name, rec.guideId);
            EmitEvent(rec, "GUIDE", Acore::StringFormat("guide '{}' complete", rec.guideId));
            rec.guideId.clear();
            rec.currentStepIndex = 0;
            rec.stepState = "idle";
            ResetObjectStepState(rec);
            PersistProgress(rec);
            return;
        }

        const GuideStep& step = guide.steps[rec.currentStepIndex];

        // Bag-full / durability guard before quest/grind/gameobject steps (Pitfall E).
        // If maintenance is being handled this tick, consume it and try again next.
        if (MaintenanceGuard(rec))
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

        // Player-like combat awareness on EVERY step, not just kill steps. Never
        // keep marching to a coordinate or an NPC while something is attacking us:
        // stop, let the class AI fight, recover if hurt, and drain loot — only
        // resume travelling/interacting once clear. (KillMobs runs its own richer
        // engage/roam machine below, so it is handled there, not here.)
        if (step.type != StepType::KillMobs)
        {
            CombatContext cc;
            _bridge->GetCombatContext(rec.guid, cc);
            if (cc.valid)
            {
                char const* rmode = nullptr;
                bool const engaged = cc.inCombat || cc.myAttackers > 0 || _bridge->IsInCombat(rec.guid);
                if (engaged)
                {
                    // Hold ground and fight back. Arm loot-grace so the kill gets
                    // looted before we move on; switch AoE by cluster size.
                    rec.lootGraceTicks = 9;
                    bool const wantAoe = cc.aoeCount >= _aoeThreshold;
                    if (wantAoe != rec.aoeOn)
                    {
                        _bridge->SetCombatStrategy(rec.guid, wantAoe ? "+aoe" : "-aoe");
                        rec.aoeOn = wantAoe;
                    }
                    // Commit to a target if the class AI has none yet, so we don't
                    // just stand there taking hits while travelling.
                    if (cc.currentTargetEntry == 0)
                    {
                        BotPosition hp;
                        uint64_t hg = 0;
                        if (_bridge->FindNearestHostile(rec.guid, 40.f, hp, hg) && hg != 0)
                            _bridge->AttackCreature(rec.guid, hg);
                    }
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
                    // Just cleared a fight during travel — grab the loot first.
                    LootAttempt const la = _bridge->LootNearby(rec.guid);
                    if (!la.hasLoot)
                        --rec.lootGraceTicks;
                    rmode = "loot";
                }

                if (rmode)
                {
                    if (_debugEnabled && (rec.dbgThrottle++ % 3 == 0))
                    {
                        BotPosition pos = _bridge->GetPosition(rec.guid);
                        LOG_INFO("module.idlebot",
                            "[IdleBot][dbg] {} step{} reactive={}: hp={:.0f}% attackers={} target={}:{}@{:.1f} pos=({:.0f},{:.0f})",
                            rec.name, rec.currentStepIndex, rmode, cc.hpPct, cc.myAttackers,
                            cc.currentTargetEntry, cc.currentTargetName, cc.currentTargetDistance,
                            pos.valid ? pos.x : 0.f, pos.valid ? pos.y : 0.f);
                    }
                    return;  // defer this step's normal action until we're clear
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
            if (!step.questId.has_value())
            {
                stepDone = true;
                break;
            }

            uint32_t objectiveCurrent = 0;
            uint32_t objectiveRequired = 0;
            stepDone = CompletionConditionMet(rec, step, &objectiveCurrent, &objectiveRequired);

            if (!stepDone)
            {
                // ---- adaptive engagement mode machine ----
                // idlebot picks the MODE from context; playerbots runs the per-class
                // rotation. Modes: recover / fight / loot / roam / engage.
                CombatContext cc;
                _bridge->GetCombatContext(rec.guid, cc);
                LootAttempt lootAttempt;
                char const* mode;

                bool const engaged = cc.inCombat || cc.myAttackers > 0 || _bridge->IsInCombat(rec.guid);
                if (cc.valid && !engaged &&
                    (cc.hpPct < static_cast<float>(_lowHpPct) ||
                     cc.manaPct < static_cast<float>(_lowManaPct)))
                {
                    // RECOVER — eat/drink when hurt or low mana, but only while SAFE.
                    // Never sit eating mid-fight (you can't, and you'd just take hits);
                    // in combat the FIGHT branch wins and the class AI handles survival.
                    mode = "recover";
                    _bridge->Recover(rec.guid);
                    rec.stuckTicks = 0;
                }
                else if (engaged)
                {
                    // FIGHT — let the class rotation work; hold position; arm loot-grace.
                    // Switch AoE on/off by cluster size (tracked to avoid strategy spam).
                    mode = "fight";
                    rec.lootGraceTicks = 9;
                    rec.stuckTicks = 0;
                    bool const wantAoe = cc.aoeCount >= _aoeThreshold;
                    if (wantAoe != rec.aoeOn)
                    {
                        _bridge->SetCombatStrategy(rec.guid, wantAoe ? "+aoe" : "-aoe");
                        rec.aoeOn = wantAoe;
                    }
                }
                else if ((lootAttempt = _bridge->LootNearby(rec.guid)).hasLoot || rec.lootGraceTicks > 0)
                {
                    // LOOT — after combat, drain visible loot before pulling again. If
                    // aggro resumes, the fight branch above takes over next tick.
                    mode = "loot";
                    if (lootAttempt.hasLoot)
                        rec.lootGraceTicks = 6;
                    else
                        --rec.lootGraceTicks;
                    rec.stuckTicks = 0;

                    if (_debugEnabled && !lootAttempt.debug.empty())
                    {
                        LOG_INFO("module.idlebot",
                            "[IdleBot][loot] {} q{} step{} acted={} hasLoot={} inRange={} corpses={} {}",
                            rec.name, *step.questId, rec.currentStepIndex,
                            lootAttempt.acted ? 1 : 0, lootAttempt.hasLoot ? 1 : 0,
                            lootAttempt.inRange ? 1 : 0, lootAttempt.lootableCorpses,
                            lootAttempt.debug);
                    }

                    if (step.type == StepType::KillMobs && lootAttempt.acted && lootAttempt.corpseEntry != 0 &&
                        std::find(step.creatureIds.begin(), step.creatureIds.end(), lootAttempt.corpseEntry) != step.creatureIds.end() &&
                        lootAttempt.corpseGuid != 0 && lootAttempt.corpseGuid != rec.lastObservedKillLootGuid)
                    {
                        rec.lastObservedKillLootGuid = lootAttempt.corpseGuid;
                        ++rec.observedKillLootsCurrentStep;
                    }
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
                else
                {
                    // ENGAGE — untapped targets in sight. Stay near the objective area and
                    // prefer the guide's configured creature list when present. Pull cap:
                    // don't add targets past MaxPull (also implicit — we don't engage while
                    // already in combat).
                    mode = "engage";
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
                    bool const haveQuestTarget = _bridge->FindNearestQuestCreature(
                        rec.guid, step.creatureIds, stepCenter, step.coords.radius, targetPos, questTargetGuid);

                    // Default: swing at the configured quest creature. We may instead
                    // pick a blocking add below so the bot fights a path to it.
                    uint64_t engageGuid = questTargetGuid;
                    BotPosition engagePos = targetPos;
                    float constexpr PullRange = 30.f;

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

                            if (!questInPull)
                            {
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

                    if (shouldAttack && engageGuid != 0 && cc.myAttackers < _maxPull)
                    {
                        bool const attacked = _bridge->AttackCreature(rec.guid, engageGuid);
                        if (attacked)
                            rec.stuckTicks = 0;
                        else if (++rec.stuckTicks > 3)
                        {
                            rec.stuckTicks = 0;
                            RoamKillObjective(rec, step);
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
            stepDone = HandleInteractGameObjectStep(rec, guide, step);
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

            BotPosition pos = _bridge->GetPosition(rec.guid);
            if (pos.valid)
            {
                rec.lastDeathMap = pos.mapId;
                rec.lastDeathX = pos.x;
                rec.lastDeathY = pos.y;
                rec.lastDeathZ = pos.z;
            }

            EmitEvent(rec, "DEATH", Acore::StringFormat("died (#{} total, #{} on this step)",
                rec.deathCountTotal, rec.deathCountStep));
            PersistProgress(rec);

            // Death loop on this step → pause for manual review.
            if (_pauseAfterDeathLoop && rec.deathCountStep >= _maxDeathsPerStep)
            {
                rec.paused = true;
                rec.stepState = "blocked";
                EmitEvent(rec, "FAILURE", Acore::StringFormat(
                    "died {} times on step {} — pausing for review",
                    rec.deathCountStep, rec.currentStepIndex + 1));
                PersistProgress(rec);
            }
            return true;
        }

        // Dead but not yet a ghost: let playerbots "auto release" handle it.
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

        // Stalled — actively nudge revive-from-corpse a few times.
        if (rec.corpseRunAttempts < _maxCorpseRunAttempts)
        {
            ++rec.corpseRunAttempts;
            rec.ghostTicks = 0;   // reset the stall window between nudges
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

        // Nothing succeeded this tick; remain dead and retry next tick.
        return true;
    }

    // -------------------------------------------------------------------------
    // Inventory awareness / town maintenance (Priority 5). Returns true if a
    // maintenance action was performed this tick (consume the tick). When the bot
    // is not near the relevant NPC the playerbot actions fail harmlessly and we
    // return false so the bot keeps pursuing its objective (no infinite block).
    // -------------------------------------------------------------------------
    bool IdleBotManager::MaintenanceGuard(BotRecord& rec)
    {
        if (!_townMaintenanceEnabled)
            return false;

        auto git = _guides.find(rec.guideId);
        if (git == _guides.end() || rec.currentStepIndex >= git->second.steps.size())
            return false;

        const GuideStep& step = git->second.steps[rec.currentStepIndex];
        bool const consumesBags =
            (step.type == StepType::KillMobs || step.type == StepType::InteractGameobject);

        InventoryStatus inv = _bridge->GetInventoryStatus(rec.guid);
        if (!inv.valid)
            return false;

        uint32_t const minFree = (step.type == StepType::KillMobs)
            ? _minFreeSlotsBeforeGrind : _minFreeSlotsBeforeQuest;
        bool const bagsLow = consumesBags && inv.freeSlots < minFree;
        bool const repairLow =
            inv.needsRepair || inv.lowestDurabilityPct < _repairBelowDurabilityPct;

        if (!bagsLow && !repairLow)
            return false;

        bool acted = false;
        if (repairLow && _bridge->Repair(rec.guid))
        {
            EmitEvent(rec, "REPAIR", Acore::StringFormat("repaired gear (was {}% durability)",
                inv.lowestDurabilityPct));
            acted = true;
        }
        if (bagsLow && _bridge->VendorTrash(rec.guid))
        {
            EmitEvent(rec, "VENDOR", Acore::StringFormat("sold trash ({} free slots before)",
                inv.freeSlots));
            acted = true;
        }
        // General maintenance (learn/restock/enchant) only matters in town; it is a
        // no-op away from the relevant NPCs.
        if ((bagsLow || repairLow) && _bridge->Maintenance(rec.guid))
            acted = true;

        return acted;
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

        // Combat positioning by class: ranged casters stand off, melee close in.
        // playerbots already applies the per-class rotation (dps/aoe/cc); we only
        // pick the positioning here, once per session.
        CombatContext cc;
        if (_bridge->GetCombatContext(rec.guid, cc) && cc.valid)
            _bridge->SetCombatStrategy(rec.guid, cc.ranged ? "+ranged" : "+close");

        rec.strategiesEnsured = true;
        LOG_DEBUG("module.idlebot", "[IdleBot] bot '{}': ensured strategies (ranged={}).", rec.name, cc.ranged);
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

        if (!rec.organicStrategiesEnsured)
        {
            // Bypass the BotActiveAlone throttle so the bot keeps questing even
            // with no real player nearby (otherwise a lone overworld bot runs
            // minimal AI), then hand questing/travel/combat to playerbots and
            // bias it quest-first (prefer quests; fall back to finding more /
            // travelling to the next hub; never autonomously grind).
            _bridge->SetForceActive(rec.guid, true);
            _bridge->SetQuestFirst(rec.guid, true);
            _bridge->SetNonCombatStrategy(rec.guid, "+grind");
            _bridge->SetNonCombatStrategy(rec.guid, "+new rpg");
            _bridge->SetNonCombatStrategy(rec.guid, "+loot");
            rec.organicStrategiesEnsured = true;

            EmitEvent(rec, "GUIDE", "switched to organic mode — questing autonomously");
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': organic mode active (+new rpg +grind +loot).", rec.name);
        }

        std::string const act = _bridge->GetRpgActivity(rec.guid);

        // Zone direction / anti-stray: if she has no quests and the autonomous AI
        // has nothing to do (idle/rest) for a sustained stretch, send her to the
        // level-appropriate hub so she picks the questing back up at the right
        // place instead of sitting or grinding. Reference, not a leash — once
        // there, the quest-first AI takes over again.
        if (rec.hubSteerCooldown > 0)
            --rec.hubSteerCooldown;

        bool const stalled = (st.questCount == 0) && (act == "idle" || act == "rest");
        rec.strayTicks = stalled ? rec.strayTicks + 1 : 0;

        // Only hub-steer at 55+, where the validated hubs are race-neutral
        // (Silithus / Outland / Northrend). Below that the hub table holds
        // race-specific starting zones, so steering could strand a mismatched
        // race (e.g. an Undead at the Blood Elf start) — let her quest in place
        // instead. The 12-55 vanilla coverage gap is tracked in NOTES.md.
        if (st.level >= 55 && rec.strayTicks > 60 && rec.hubSteerCooldown == 0)
        {
            LevelHub hub;
            if (NextHubFor(_bridge->GetTeamId(rec.guid), st.level, hub))
            {
                float const dx = st.pos.x - hub.x;
                float const dy = st.pos.y - hub.y;
                bool const farFromHub = !st.pos.valid || st.pos.mapId != hub.mapId ||
                    (dx * dx + dy * dy) > (400.f * 400.f);
                if (farFromHub)
                {
                    _bridge->TeleportBot(rec.guid, hub.mapId, hub.x, hub.y, hub.z);
                    EmitEvent(rec, "TRAVEL", Acore::StringFormat(
                        "out of quests — heading to {} (L{}+ hub)", hub.zone, hub.minLevel));
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
                "[IdleBot][organic] {} L{} hp={}% quests={} doing={} stray={} pos=({:.0f},{:.0f}) map={}",
                rec.name, st.level,
                st.maxHealth ? (st.health * 100u / st.maxHealth) : 0u,
                st.questCount, act, rec.strayTicks,
                st.pos.valid ? st.pos.x : 0.f, st.pos.valid ? st.pos.y : 0.f,
                st.pos.mapId);
        }
        return true;
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
            // Observed-loot fallback only counts while the bot actually holds the
            // quest — otherwise looted corpses from an unaccepted quest could fake
            // step completion (masks a failed accept rather than failing loudly).
            QuestState const qs = _bridge->GetQuestStatus(rec.guid, questId);
            if (qs == QuestState::InProgress || qs == QuestState::Complete)
                outCurrent = std::max<uint32_t>(outCurrent, rec.observedKillLootsCurrentStep);

            return hasBridgeProgress || outRequired > 0;
        }

        return hasBridgeProgress;
    }

    bool IdleBotManager::CompletionConditionMet(BotRecord& rec, GuideStep const& step, uint32_t* outCurrent, uint32_t* outRequired) const
    {
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

    void IdleBotManager::RoamKillObjective(BotRecord& rec, GuideStep const& step)
    {
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
            if (_bridge->FindNearestQuestCreature(rec.guid, step.creatureIds, center, step.coords.radius, targetPos, targetGuid) &&
                targetPos.valid)
            {
                _bridge->MoveTo(rec.guid, targetPos.mapId, targetPos.x, targetPos.y, targetPos.z, 5.f);
                return;
            }
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

    // Emit a categorized IdleRPG event to the per-bot log and (optionally) the
    // idlebot_events table. bot_id is resolved by subselect to stay decoupled.
    void IdleBotManager::EmitEvent(const BotRecord& rec, const char* category, const std::string& message)
    {
        sIdleBotLog->Write(rec.name, category, message);
        if (_eventsToDb)
        {
            CharacterDatabase.Execute(
                "INSERT INTO idlebot_events (bot_id, event_type, detail) "
                "SELECT id, '{}', '{}' FROM idlebot_bots WHERE bot_name = '{}'",
                SqlEscape(category), SqlEscape(message), SqlEscape(rec.name));
        }
    }

    // Persist guide progress + death counters so a restart resumes cleanly (Priority 3).
    void IdleBotManager::PersistProgress(const BotRecord& rec)
    {
        std::string const guideClause =
            rec.guideId.empty() ? std::string("NULL") : ("'" + SqlEscape(rec.guideId) + "'");
        CharacterDatabase.Execute(
            "UPDATE idlebot_bots SET guide_id = {}, step_index = {}, step_state = '{}', "
            "death_count_total = {}, death_count_current_step = {} WHERE bot_name = '{}'",
            guideClause, rec.currentStepIndex, SqlEscape(rec.stepState),
            rec.deathCountTotal, rec.deathCountStep, SqlEscape(rec.name));
    }

    // Advance to the next guide step: fresh per-step death budget + persist.
    void IdleBotManager::AdvanceStep(BotRecord& rec)
    {
        ++rec.currentStepIndex;
        rec.deathCountStep = 0;
        rec.stepState = "idle";
        rec.observedKillLootsCurrentStep = 0;
        rec.lastObservedKillLootGuid = 0;
        ResetObjectStepState(rec);
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
                        EmitEvent(rec, "QUEST", Acore::StringFormat("quest progress {}/{}", currentCount, requiredCount));
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
            EmitEvent(rec, "GUIDE", "optional object step timed out; skipping");
            ResetObjectStepState(rec);
            return true;
        }

        if (rec.lastObjectRetryMs >= _gameObjectRetryEveryMs)
        {
            EmitEvent(rec, "OBJECT", "No object available; waiting for respawn.");
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

        if (_gameObjectWaitForRespawn && maxWaitMs > 0 && rec.objectWaitMs >= maxWaitMs && !skippable)
        {
            if (rec.lastObjectFailureReason != "required_wait")
            {
                rec.lastObjectFailureReason = "required_wait";
                EmitEvent(rec, "OBJECT", "Waiting for respawn on required object step.");
            }
        }

        return false;
    }

    void IdleBotManager::LoadBots()
    {
        // Best-effort: if the idlebot_bots table is absent the query returns null
        // and we simply start with an empty registry. Restores guide progress so a
        // worldserver restart resumes mid-guide rather than from step 0 (Priority 3).
        QueryResult result = CharacterDatabase.Query(
            "SELECT bot_name, active, guide_id, step_index, step_state, "
            "death_count_total, death_count_current_step, decision_mode FROM idlebot_bots");
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
        sIdleBotLog->Write(name, "EVENT", "paused");
        return true;
    }

    bool IdleBotManager::ResumeBot(const std::string& rawName)
    {
        std::string const name = NormalizeName(rawName);
        auto it = _bots.find(name);
        if (it == _bots.end()) return false;
        it->second.paused = false;
        // Resuming clears a death-loop block so the bot tries the step again.
        if (it->second.stepState == "blocked")
        {
            it->second.stepState = "idle";
            it->second.deathCountStep = 0;
            PersistProgress(it->second);
        }
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
        BotRecord& rec = bit->second;
        rec.guideId = guideId;
        rec.currentStepIndex = 0;
        rec.stepState = "idle";
        rec.deathCountStep = 0;
        ResetObjectStepState(rec);
        PersistProgress(rec);
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
}
