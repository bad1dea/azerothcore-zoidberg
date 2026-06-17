#include "IdleBotPlayerbotBridge.h"
#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include "Bag.h"
#include "Item.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "MotionMaster.h"
#include "WorldSession.h"
#include "Log.h"

#include <algorithm>

// The "internal" bridge: drives bots via direct mod-playerbots calls.
//
// NOTE ON STRATEGY: CLAUDE.md's prototype plan was to drive bots via
// `.playerbots` CHAT commands. That isn't viable for idlebot: issuing those
// commands requires a master WorldSession/ChatHandler, and idlebot has no human
// master. mod-playerbots supports a MASTER-LESS add — AddPlayerBot(guid, 0) sets
// isRndbot and routes the login through RandomPlayerbotMgr with no master needed
// (verified: PlayerbotMgr.cpp AddPlayerBot + HandlePlayerBotLoginCallback, which
// does NOT gate on the character being a random-bot account). So the bridge uses
// that direct call. The seam (IdleBotPlayerbotBridge) is unchanged; the decision
// engine / executor above it never see this.
//
// Coupling to mod-playerbots is confined to this file and guarded by
// MOD_PLAYERBOTS (defined for the modules target when the playerbots module is
// present). Bot DETECTION uses the core WorldSession::IsBot(), so it needs no
// playerbots header.

#ifdef MOD_PLAYERBOTS
#include "RandomPlayerbotMgr.h"   // sRandomPlayerbotMgr, AddPlayerBot, LogoutPlayerBot
#include "Script/Playerbots.h"    // GET_PLAYERBOT_AI
#include "Bot/PlayerbotAI.h"      // PlayerbotAI, DoSpecificAction, IsRanged
#include "AiObjectContext.h"      // GetValue<T>("possible targets"/"aoe count"/...)
#endif

namespace idlebot
{
    namespace
    {
        ObjectGuid ResolveGuid(std::string const& name)
        {
            return sCharacterCache->GetCharacterGuidByName(name);
        }

        Player* ResolvePlayer(BotGuid raw)
        {
            if (!raw)
                return nullptr;
            return ObjectAccessor::FindConnectedPlayer(ObjectGuid(raw));
        }

        // Returns an in-world Player only if it is currently playerbot-controlled,
        // else nullptr. Death/recovery and action calls require this.
        Player* ResolveOnlinePlayer(BotGuid raw)
        {
            Player* p = ResolvePlayer(raw);
            return (p && p->IsInWorld()) ? p : nullptr;
        }
    }

    class IdleBotInternalBridge : public IdleBotPlayerbotBridge
    {
    public:
        // --- lifecycle ---
        bool EnsureBotOnline(std::string const& botName) override
        {
            ObjectGuid guid = ResolveGuid(botName);
            if (!guid)
            {
                LOG_WARN("module.idlebot", "[IdleBot] EnsureBotOnline: no character named '{}'.", botName);
                return false;
            }

            Player* p = ObjectAccessor::FindConnectedPlayer(guid);
            if (p && p->IsInWorld())
                return true;   // already online

#ifdef MOD_PLAYERBOTS
            // Master-less add. RandomPlayerbotMgr drives the bot's AI update loop
            // (combat etc.) without a human master. NOTE: the self-account "altbot"
            // approach was tried and reverted — without an online master the altbot
            // is not ticked and goes idle. The bot is NOT in the random-bot pool
            // (real char on a dedicated account), so it is not treated as a pool
            // random bot for behaviour; looting is handled by letting the grind/loot
            // strategy run uninterrupted (idlebot must not drag it off corpses).
            sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
            LOG_INFO("module.idlebot", "[IdleBot] queued login for '{}'.", botName);
            return true;
#else
            LOG_WARN("module.idlebot", "[IdleBot] cannot bring '{}' online: built without mod-playerbots.", botName);
            return false;
#endif
        }

        bool ReleaseBot(std::string const& botName) override
        {
            ObjectGuid guid = ResolveGuid(botName);
            if (!guid)
                return false;

            if (!ObjectAccessor::FindConnectedPlayer(guid))
                return false;   // not online

#ifdef MOD_PLAYERBOTS
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
            LOG_INFO("module.idlebot", "[IdleBot] released bot '{}'.", botName);
            return true;
#else
            return false;
#endif
        }

        BotGuid GetBotGuid(std::string const& botName) override
        {
            return ResolveGuid(botName).GetRawValue();   // 0 if unknown
        }

        bool GetLiveStatus(BotGuid bot, BotLiveStatus& out) override
        {
            out = BotLiveStatus{};

            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return true;   // resolvable but offline -> online=false

            out.online     = true;
            out.controlled = p->GetSession() && p->GetSession()->IsBot();
            out.level      = p->GetLevel();
            out.health     = p->GetHealth();
            out.maxHealth  = p->GetMaxHealth();
            out.mana       = p->GetPower(POWER_MANA);
            out.maxMana    = p->GetMaxPower(POWER_MANA);

            uint32_t quests = 0;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (p->GetQuestSlotQuestId(slot))
                    ++quests;
            out.questCount = quests;

            out.pos.mapId = p->GetMapId();
            out.pos.x = p->GetPositionX();
            out.pos.y = p->GetPositionY();
            out.pos.z = p->GetPositionZ();
            out.pos.o = p->GetOrientation();
            out.pos.valid = true;
            return true;
        }

        // --- state reads ---
        BotPosition GetPosition(BotGuid bot) override
        {
            BotPosition pos;
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return pos;
            pos.mapId = p->GetMapId();
            pos.x = p->GetPositionX();
            pos.y = p->GetPositionY();
            pos.z = p->GetPositionZ();
            pos.o = p->GetOrientation();
            pos.valid = true;
            return pos;
        }

        uint32_t GetLevel(BotGuid bot) override
        {
            Player* p = ResolvePlayer(bot);
            return p ? p->GetLevel() : 0;
        }

        bool IsDead(BotGuid bot) override
        {
            Player* p = ResolvePlayer(bot);
            return p ? !p->IsAlive() : false;
        }

        // Populate free/total slots and lowest equipped durability.
        // Verified: Player::GetFreeInventorySpace() (Player.h:1265),
        //   Player::GetBagByPos(slot) (Player.h:1264), Bag::GetBagSize() (Bag.h:48),
        //   Item::GetUInt32Value(ITEM_FIELD_DURABILITY/MAXDURABILITY) (Item.h:257).
        InventoryStatus GetInventoryStatus(BotGuid bot) override
        {
            InventoryStatus inv;
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return inv;

            inv.valid = true;
            inv.freeSlots = p->GetFreeInventorySpace();

            // Backpack is always 16 slots; add equipped bag capacities.
            uint32_t total = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START; // 16
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
                if (Bag* bag = p->GetBagByPos(slot))
                    total += bag->GetBagSize();
            inv.totalSlots = total;

            // Lowest durability percentage across equipped, damageable items.
            uint32_t lowestPct = 100;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            {
                Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item)
                    continue;
                uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
                if (maxDur == 0)
                    continue;   // not damageable
                uint32 dur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
                uint32 pct = (dur * 100) / maxDur;
                if (pct < lowestPct)
                    lowestPct = pct;
                if (dur == 0)
                    inv.needsRepair = true;
            }
            inv.lowestDurabilityPct = lowestPct;
            return inv;
        }

        void GetXp(BotGuid bot, uint32_t& outXp, uint32_t& outXpForNextLevel) override
        {
            outXp = 0;
            outXpForNextLevel = 0;
            if (Player* p = ResolveOnlinePlayer(bot))
            {
                outXp = p->GetUInt32Value(PLAYER_XP);
                outXpForNextLevel = p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
            }
        }

        // Map AzerothCore QuestStatus → idlebot QuestState.
        // Verified: QUEST_STATUS_* in src/server/game/Quests/QuestDef.h:98
        QuestState GetQuestStatus(BotGuid bot, uint32_t questId) override
        {
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return QuestState::Unknown;

            switch (p->GetQuestStatus(questId))
            {
            case QUEST_STATUS_NONE:       return QuestState::NotStarted;
            case QUEST_STATUS_INCOMPLETE: return QuestState::InProgress;
            case QUEST_STATUS_COMPLETE:   return QuestState::Complete;
            case QUEST_STATUS_REWARDED:   return QuestState::Rewarded;
            case QUEST_STATUS_FAILED:     return QuestState::Failed;
            default:                      return QuestState::Unknown;
            }
        }

        // --- M3 executor actions ---

        // Queue movement to (mapId, x, y, z). Returns true if the move command
        // was issued (or the bot is already within radius). Arrival is checked on
        // the next tick via GetPosition; do not poll here.
        bool MoveTo(BotGuid bot, uint32_t mapId, float x, float y, float z, float /*radius*/) override
        {
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            if (p->GetMapId() != mapId)
            {
                LOG_WARN("module.idlebot", "[IdleBot] MoveTo: bot is on map {} but step wants map {}.", p->GetMapId(), mapId);
                return false;
            }

            // Don't fight the playerbots AI over movement while the bot is in combat —
            // the combat engine owns the motion master during a fight.
            if (p->IsInCombat())
                return true;

            // Raw MovePoint. The bot AI may override this on its next frame if it
            // decides to move somewhere itself (e.g. move random / travel strategy).
            // This is a known M4 limitation; proper playerbots TravelMgr integration
            // is deferred.
            // Verified: MotionMaster::MovePoint(uint32 id, float x, float y, float z, ...)
            // in src/server/game/Movement/MotionMaster.h:242
            p->GetMotionMaster()->MovePoint(0, x, y, z);
            return true;
        }

        // Returns true if the bot is within INTERACTION_DISTANCE of a creature
        // with the given entry. The executor uses this to gate AcceptQuest/TurnInQuest.
        bool InteractWithNpc(BotGuid bot, uint64_t npcEntry32) override
        {
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            Creature* c = p->FindNearestCreature(static_cast<uint32_t>(npcEntry32), 5.5f /*INTERACTION_DISTANCE*/);
            return c != nullptr;
        }

        // Accept questId from the nearest alive creature with entry npcEntry32.
        // Verified: Player::AddQuestAndCheckCompletion(Quest const*, Object*)
        // in src/server/game/Entities/Player/Player.h:1458
        bool AcceptQuest(BotGuid bot, uint32_t questId, uint64_t npcEntry32) override
        {
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                LOG_WARN("module.idlebot", "[IdleBot] AcceptQuest: unknown quest id {}.", questId);
                return false;
            }

            if (!p->CanAddQuest(quest, false))
                return false;

            Creature* npc = p->FindNearestCreature(static_cast<uint32_t>(npcEntry32), 5.5f);
            p->AddQuestAndCheckCompletion(quest, npc);   // npc may be nullptr
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': accepted quest {}.", p->GetName(), questId);
            return true;
        }

        // Turn in questId to the nearest alive creature with entry npcEntry32.
        // Verified: Player::RewardQuest(Quest const*, uint32 reward, Object*, bool announce, bool isLFG)
        // in src/server/game/Entities/Player/Player.h:1463
        bool TurnInQuest(BotGuid bot, uint32_t questId, uint64_t npcEntry32) override
        {
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                LOG_WARN("module.idlebot", "[IdleBot] TurnInQuest: unknown quest id {}.", questId);
                return false;
            }

            if (!p->CanRewardQuest(quest, false))
                return false;

            Creature* npc = p->FindNearestCreature(static_cast<uint32_t>(npcEntry32), 5.5f);
            p->RewardQuest(quest, 0 /*first reward choice*/, npc, true /*announce*/);
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': turned in quest {}.", p->GetName(), questId);
            return true;
        }

        // --- generic playerbots seam ---

        // Run a named playerbots action silently. THE single point of coupling for
        // higher-level behaviour (release/revive/repair/loot/maintenance/...).
        bool DoBotAction(BotGuid bot, std::string const& actionName) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return false;
            return botAI->DoSpecificAction(actionName, Event(), true /*silent*/);
#else
            (void)bot; (void)actionName;
            return false;
#endif
        }

        bool SetNonCombatStrategy(BotGuid bot, std::string const& strategyExpr) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return false;
            botAI->ChangeStrategy(strategyExpr, BOT_STATE_NON_COMBAT);
            return true;
#else
            (void)bot; (void)strategyExpr;
            return false;
#endif
        }

        bool SetCombatStrategy(BotGuid bot, std::string const& strategyExpr) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return false;
            botAI->ChangeStrategy(strategyExpr, BOT_STATE_COMBAT);
            return true;
#else
            (void)bot; (void)strategyExpr;
            return false;
#endif
        }

        // Tell the bot to attack the nearest viable mob via its own grind targeting.
        // creatureGuid is ignored (0 = pick nearest); the bot AI's GrindTargetValue
        // handles quest-need prioritisation and level/range checks.
        bool AttackCreature(BotGuid bot, uint64_t /*creatureGuid*/) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || p->IsInCombat())
                return false;
            // "attack anything" → AttackAnythingAction → GrindTargetValue picks the
            // nearest hostile mob (quest-needed mobs prioritised).
            return DoBotAction(bot, "attack anything");
        }

        // --- world-object lookups + gameobject interaction ---

        uint64_t FindNearestCreatureEntry(BotGuid bot, uint32_t entry, float radius) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return 0;
            Creature* c = p->FindNearestCreature(entry, radius);
            return c ? c->GetGUID().GetRawValue() : 0;
        }

        bool FindNearestQuestCreaturePos(BotGuid bot, std::vector<uint32_t> const& entries, float radius, BotPosition& out) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            Creature* best = nullptr;
            float bestDist = 0.f;
            for (uint32_t e : entries)
            {
                Creature* c = p->FindNearestCreature(e, radius);   // nearest alive
                if (!c)
                    continue;
                float d = p->GetDistance(c);
                if (!best || d < bestDist)
                {
                    best = c;
                    bestDist = d;
                }
            }
            if (!best)
                return false;
            out.mapId = best->GetMapId();
            out.x = best->GetPositionX();
            out.y = best->GetPositionY();
            out.z = best->GetPositionZ();
            out.valid = true;
            return true;
        }

        uint64_t FindNearestGameObjectEntry(BotGuid bot, uint32_t entry, float radius) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return 0;
            GameObject* go = p->FindNearestGameObject(entry, radius, true);
            return go ? go->GetGUID().GetRawValue() : 0;
        }

        bool FindNearestGameObjectPosition(BotGuid bot, uint32_t entry, float radius, BotPosition& out) override
        {
            out = BotPosition{};
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;

            GameObject* go = p->FindNearestGameObject(entry, radius, true);
            if (!go)
                return false;

            out.mapId = go->GetMapId();
            out.x = go->GetPositionX();
            out.y = go->GetPositionY();
            out.z = go->GetPositionZ();
            out.o = go->GetOrientation();
            out.valid = true;
            return true;
        }

        QuestObjectiveProgress GetQuestObjectiveProgress(BotGuid bot, uint32_t questId, uint32_t gameobjectEntry, uint32_t itemEntry) override
        {
            QuestObjectiveProgress out;
            Player* p = ResolveOnlinePlayer(bot);
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!p || !quest)
                return out;

            QuestStatusMap::const_iterator it = p->getQuestStatusMap().find(questId);
            if (it == p->getQuestStatusMap().end())
                return out;

            QuestStatusData const& data = it->second;
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                if (quest->RequiredNpcOrGo[i] == -static_cast<int32>(gameobjectEntry) && quest->RequiredNpcOrGoCount[i] > 0)
                {
                    out.current = data.CreatureOrGOCount[i];
                    out.required = quest->RequiredNpcOrGoCount[i];
                    out.complete = out.current >= out.required;
                    out.valid = true;
                    return out;
                }
            }

            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            {
                if (quest->RequiredItemId[i] && (!itemEntry || quest->RequiredItemId[i] == itemEntry))
                {
                    out.current += data.ItemCount[i];
                    out.required += quest->RequiredItemCount[i];
                    out.valid = true;
                }
            }

            if (out.valid)
                out.complete = out.required > 0 && out.current >= out.required;
            return out;
        }

        bool IsNearGameObject(BotGuid bot, uint32_t entry, float radius) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            return p->FindNearestGameObject(entry, radius, true) != nullptr;
        }

        // Right-click the nearest gameobject of `entry`. Private-server-direct:
        // GameObject::Use(player) drives the same path a client click would
        // (loot chest / quest credit / goober). Returns false if none in range.
        // Verified: Object::FindNearestGameObject (Object.h:641),
        //   GameObject::Use(Unit*) (GameObject.h:222).
        bool UseGameObject(BotGuid bot, uint32_t entry, float radius) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            GameObject* go = p->FindNearestGameObject(entry, radius, true);
            if (!go)
                return false;
            go->Use(p);
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': used gameobject entry {} ({}).",
                p->GetName(), entry, go->GetGUID().ToString());
            return true;
        }

        bool LootGameObject(BotGuid bot, uint32_t entry, float radius) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;

            GameObject* go = p->FindNearestGameObject(entry, radius, true);
            if (!go)
                return false;

            p->SendLoot(go->GetGUID(), LOOT_CORPSE);

            Loot* loot = &go->loot;
            uint32 const slotCount = std::min<uint32>(loot->items.size() + loot->quest_items.size(), 255);
            bool storedAny = false;

            for (uint32 slot = 0; slot < slotCount; ++slot)
            {
                InventoryResult result = EQUIP_ERR_OK;
                LootItem* stored = p->StoreLootItem(static_cast<uint8>(slot), loot, result);
                if (stored && result == EQUIP_ERR_OK)
                    storedAny = true;
            }

            if (p->GetLootGUID() == go->GetGUID())
                p->GetSession()->DoLootRelease(go->GetGUID());

            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': attempted gameobject loot entry {} ({}), stored={}.",
                p->GetName(), entry, go->GetGUID().ToString(), storedAny ? "true" : "false");
            return storedAny;
        }

        // --- maintenance (routed through playerbots actions) ---
        bool LootNearby(BotGuid bot) override  { return DoBotAction(bot, "loot"); }
        bool VendorTrash(BotGuid bot) override { return DoBotAction(bot, "sell"); }
        bool Repair(BotGuid bot) override      { return DoBotAction(bot, "repair"); }
        bool Train(BotGuid bot) override       { return DoBotAction(bot, "trainer"); }
        bool Maintenance(BotGuid bot) override { return DoBotAction(bot, "maintenance"); }

        bool IsInCombat(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p ? p->IsInCombat() : false;
        }

        // Read playerbots' own combat values so idlebot can pick an engagement mode.
        // Verified: PlayerbotAI::IsRanged(Player*) (PlayerbotAI.h:422); values
        // "possible targets" (tap/LoS-filtered), "my attackers count", "aoe count"
        // via AiObjectContext::GetValue<T>(name)->Get().
        bool GetCombatContext(BotGuid bot, CombatContext& out) override
        {
            out = CombatContext{};
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;

            out.inCombat = p->IsInCombat();
            out.hpPct    = p->GetHealthPct();
            out.manaPct  = p->GetMaxPower(POWER_MANA) ? p->GetPowerPct(POWER_MANA) : 100.f;
            out.ranged   = PlayerbotAI::IsRanged(p);

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (botAI && botAI->GetAiObjectContext())
            {
                AiObjectContext* ctx = botAI->GetAiObjectContext();
                out.possibleTargets = static_cast<uint32_t>(
                    ctx->GetValue<GuidVector>("possible targets")->Get().size());
                out.myAttackers = ctx->GetValue<uint8>("my attackers count")->Get();
                out.aoeCount    = ctx->GetValue<uint8>("aoe count")->Get();
            }
            out.valid = true;
            return true;
#else
            (void)bot;
            return false;
#endif
        }

        // Eat/drink to restore. Playerbots' grind strategy exposes "food"/"drink".
        bool Recover(BotGuid bot) override
        {
            bool const ate  = DoBotAction(bot, "food");
            bool const drank = DoBotAction(bot, "drink");
            return ate || drank;
        }

        uint32_t GetMoney(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p ? p->GetMoney() : 0;
        }

        // --- death / recovery ---
        bool IsGhost(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p ? p->HasPlayerFlag(PLAYER_FLAGS_GHOST) : false;
        }

        bool RequestReleaseSpirit(BotGuid bot) override      { return DoBotAction(bot, "release"); }
        bool RequestReviveFromCorpse(BotGuid bot) override   { return DoBotAction(bot, "revive from corpse"); }
        bool RequestSpiritHealerRevive(BotGuid bot) override { return DoBotAction(bot, "spirit healer"); }

        // Direct core resurrect — private-server convenience fallback only. Caller
        // must gate on config. Verified: Player::ResurrectPlayer(float, bool),
        //   SpawnCorpseBones(bool), RepopAtGraveyard() (Player.h:2058-2068).
        bool DirectResurrect(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || p->IsAlive())
                return false;

            // If the corpse hasn't been created yet (still a fresh body), build the
            // repop so we have a ghost we can resurrect cleanly.
            if (!p->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !p->GetCorpse())
                p->BuildPlayerRepop();

            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': direct-resurrected (private-server fallback).", p->GetName());
            return true;
        }

        bool ReviveOrCorpseRun(BotGuid bot) override
        {
            // Player-like: nudge the playerbots dead-state recovery. The DeadStrategy
            // already auto-releases + corpse-runs; this re-asserts revive-from-corpse
            // and falls back to the spirit healer if the corpse run has stalled.
            if (RequestReviveFromCorpse(bot))
                return true;
            return RequestSpiritHealerRevive(bot);
        }

        // --- not yet implemented ---
        bool FollowPlayer(BotGuid, PlayerGuid) override { return false; }
        bool CastSpell(BotGuid, uint32_t, uint64_t) override { return false; }

        std::vector<PlayerGuid> GetNearbyPlayers(BotGuid, float) override { return {}; }
        std::vector<uint64_t> GetNearbyCreatures(BotGuid, float) override { return {}; }
        std::vector<uint64_t> GetNearbyGameObjects(BotGuid, float) override { return {}; }

        bool InvitePlayer(BotGuid, PlayerGuid) override { return false; }
        bool AcceptGroupInvite(BotGuid, PlayerGuid) override { return false; }
        bool LeaveGroup(BotGuid) override { return false; }
    };

    IdleBotPlayerbotBridge* CreateBridge(std::string const& /*controlMode*/)
    {
        // Only the internal bridge exists today; a chat-command variant is not
        // viable master-less (see file header). controlMode is reserved.
        return new IdleBotInternalBridge();
    }
}
