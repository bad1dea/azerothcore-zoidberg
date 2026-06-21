#include "IdleBotPlayerbotBridge.h"
#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Bag.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "MotionMaster.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <list>
#include <sstream>

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
#include "Playerbots.h"           // GET_PLAYERBOT_AI
#include "PlayerbotAI.h"          // PlayerbotAI, DoSpecificAction, IsRanged
#include "PlayerbotFactory.h"     // talent auto-spec (InitTalentsTree)
#include "AiObjectContext.h"      // GetValue<T>("possible targets"/"aoe count"/...)
#include "LootObjectStack.h"
#include "LootAction.h"           // StoreLootAction::IsLootAllowed
#endif

namespace idlebot
{
    namespace
    {
        constexpr float IdleBotLootSearchRadius = 45.f;

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

        Creature* FindQuestNpc(Player* p, uint32 entry, uint32 questId, bool turnIn)
        {
            if (!p || !entry)
                return nullptr;

            std::list<Creature*> creatures;
            p->GetCreatureListWithEntryInGrid(creatures, entry, 8.0f);
            Creature* best = nullptr;
            float bestDistance = std::numeric_limits<float>::max();

            for (Creature* creature : creatures)
            {
                if (!creature)
                    continue;

                if (turnIn)
                {
                    if (!creature->hasInvolvedQuest(questId))
                        continue;
                }
                else if (!creature->hasQuest(questId))
                    continue;

                if (!p->CanInteractWithQuestGiver(creature))
                    continue;

                float const dist = p->GetDistance(creature);
                if (dist < bestDistance)
                {
                    best = creature;
                    bestDistance = dist;
                }
            }

            return best;
        }

#ifdef MOD_PLAYERBOTS
        std::string ItemName(uint32 itemId)
        {
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
                return proto->Name1;
            return "unknown";
        }

        void MarkBotManaged(ObjectGuid guid, bool active)
        {
            if (!guid)
                return;

            uint32 const botId = guid.GetCounter();
            if (!active)
            {
                sRandomPlayerbotMgr.SetValue(botId, "add", 0);
                sRandomPlayerbotMgr.SetValue(botId, "logout", 0);
                return;
            }

            sRandomPlayerbotMgr.SetValue(botId, "add", 1);
            sRandomPlayerbotMgr.SetValue(botId, "logout", 0);

            // IdleBot owns travel/quest flow. Suppress random-bot maintenance loops
            // that would otherwise re-randomize gear/quests or teleport the bot away.
            sRandomPlayerbotMgr.SetValue(botId, "randomize", 1);
            sRandomPlayerbotMgr.SetValue(botId, "teleport", 1);
            sRandomPlayerbotMgr.SetValue(botId, "change_strategy", 1);
        }

        void DescribeCreatureLoot(Player* p, PlayerbotAI* botAI, Creature* c, LootAttempt& out)
        {
            if (!p || !botAI || !c)
                return;

            bool const lootableFlag = c->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            bool const allowed = lootableFlag && p->isAllowedToLoot(c);
            float const dist = p->GetDistance(c);

            if (allowed)
            {
                ++out.lootableCorpses;
                out.hasLoot = true;
            }

            if (!out.debug.empty())
                out.debug += " | ";

            std::ostringstream line;
            line << c->GetEntry() << ":" << c->GetName() << "@" << std::fixed << std::setprecision(1) << dist
                 << " flag=" << (lootableFlag ? 1 : 0)
                 << " allowed=" << (allowed ? 1 : 0)
                 << " gold=" << c->loot.gold
                 << " unlooted=" << uint32(c->loot.unlootedCount);

            uint32 const maxSlot = c->loot.GetMaxSlotInLootFor(p);
            uint32 described = 0;
            uint32 hidden = 0;
            for (uint32 i = 0; i < maxSlot; ++i)
            {
                LootItem* item = c->loot.LootItemInSlot(i, p);
                if (!item)
                    continue;

                if (described >= 4)
                {
                    ++hidden;
                    continue;
                }

                bool const allowedByPlayerbots = StoreLootAction::IsLootAllowed(item->itemid, botAI);
                line << " item" << i << "=" << item->itemid << ":" << ItemName(item->itemid)
                     << "x" << uint32(item->count)
                     << (allowedByPlayerbots ? ":take" : ":skip");
                ++described;
            }

            if (hidden > 0)
                line << " +" << hidden << "more";

            out.debug += line.str();
        }

        void AppendQueuedLootDebug(LootAttempt& out, std::string const& text)
        {
            if (!out.debug.empty())
                out.debug += " ; ";
            out.debug += text;
        }

#endif
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
            if (p)
                return true;   // already connected; let GetLiveStatus wait for full world entry / AI attach

#ifdef MOD_PLAYERBOTS
            // Master-less add. RandomPlayerbotMgr drives the bot's AI update loop
            // (combat etc.) without a human master. NOTE: the self-account "altbot"
            // approach was tried and reverted — without an online master the altbot
            // is not ticked and goes idle. The bot is NOT in the random-bot pool
            // (real char on a dedicated account), so it is not treated as a pool
            // random bot for behaviour; looting is handled by letting the grind/loot
            // strategy run uninterrupted (idlebot must not drag it off corpses).
            MarkBotManaged(guid, true);
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
            MarkBotManaged(guid, false);
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
            if (!p)
                return true;   // resolvable but offline -> online=false

            // A bot can be connected before it has fully entered the world. Treat
            // that as online-but-not-ready so IdleBot waits instead of re-queuing
            // duplicate logins for the same session.
            if (!p->IsInWorld())
            {
                out.online = true;
                return true;
            }

            out.online     = true;
#ifdef MOD_PLAYERBOTS
            MarkBotManaged(p->GetGUID(), true);
            // "controlled" REQUIRES a live PlayerbotAI. If the bot is connected
            // but the AI object is missing, reuse mod-playerbots' own login path
            // to recreate the AI in-place instead of waiting for relog churn.
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
            {
                sRandomPlayerbotMgr.OnBotLogin(p);
                botAI = GET_PLAYERBOT_AI(p);
            }

            out.controlled = botAI && !botAI->IsRealPlayer();
#else
            out.controlled = p->GetSession() && p->GetSession()->IsBot();
#endif
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

        void TeleportBot(BotGuid bot, uint32_t mapId, float x, float y, float z) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || p->IsInCombat() || p->IsInFlight())
                return;
            p->TeleportTo(mapId, x, y, z + 3.0f, p->GetOrientation());
        }

        uint8_t GetTeamId(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return 0;
            return p->GetTeamId() == TEAM_HORDE ? 1 : 0;
        }

        uint8_t GetClass(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p ? p->getClass() : 0;
        }

        uint8_t GetRace(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p ? p->getRace() : 0;
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

            if (!p->CanTakeQuest(quest, false) || !p->CanAddQuest(quest, false))
                return false;

            Creature* npc = FindQuestNpc(p, static_cast<uint32_t>(npcEntry32), questId, false);
            if (!npc)
            {
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': accept quest {} blocked npc={} near=0 canTake=1 canAdd=1.",
                    p->GetName(), questId, static_cast<uint32_t>(npcEntry32));
                return false;
            }

            WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST);
            uint32_t unknown = 0;
            packet << npc->GetGUID() << questId << unknown;
            packet.rpos(0);
            p->GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);

            uint32_t questCount = 0;
            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                if (p->GetQuestSlotQuestId(slot))
                    ++questCount;

            QuestStatus const status = p->GetQuestStatus(questId);
            uint16 const slot = p->FindQuestSlot(questId);
            bool const accepted = status != QUEST_STATUS_NONE && status != QUEST_STATUS_REWARDED &&
                slot < MAX_QUEST_LOG_SIZE;
            if (accepted)
                p->SaveToDB(false, false);

            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': accept quest {} result status={} slot={} quests={} npc={} near={}.",
                p->GetName(), questId, static_cast<uint32_t>(status),
                slot < MAX_QUEST_LOG_SIZE ? static_cast<int32>(slot) : -1,
                questCount, static_cast<uint32_t>(npcEntry32), npc ? 1 : 0);
            return accepted;
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

            if (npcEntry32 == 0)
            {
                p->RewardQuest(quest, 0 /*first reward choice*/, nullptr, true /*announce*/);
                if (p->GetQuestStatus(questId) == QUEST_STATUS_REWARDED)
                    p->SaveToDB(false, false);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': turned in quest {} without explicit questgiver.",
                    p->GetName(), questId);
                return p->GetQuestStatus(questId) == QUEST_STATUS_REWARDED;
            }

            Creature* npc = FindQuestNpc(p, static_cast<uint32_t>(npcEntry32), questId, true);
            if (!npc)
            {
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': turn-in quest {} blocked npc={} near=0.",
                    p->GetName(), questId, static_cast<uint32_t>(npcEntry32));
                return false;
            }

            WorldPacket packet(CMSG_QUESTGIVER_CHOOSE_REWARD);
            packet << npc->GetGUID() << questId << uint32_t(0);
            packet.rpos(0);
            p->GetSession()->HandleQuestgiverChooseRewardOpcode(packet);
            if (p->GetQuestStatus(questId) == QUEST_STATUS_REWARDED)
                p->SaveToDB(false, false);

            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': turned in quest {}.", p->GetName(), questId);
            return p->GetQuestStatus(questId) == QUEST_STATUS_REWARDED;
        }

        std::vector<uint32_t> GetCompletedQuests(BotGuid bot) override
        {
            std::vector<uint32_t> out;
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return out;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = p->GetQuestSlotQuestId(slot);
                if (qid && p->GetQuestStatus(qid) == QUEST_STATUS_COMPLETE)
                    out.push_back(qid);
            }
            return out;
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

        void SetForceActive(BotGuid bot, bool on) override
        {
#ifdef MOD_PLAYERBOTS
            PlayerbotAI::SetForceActive(ObjectGuid(bot), on);
#else
            (void)bot; (void)on;
#endif
        }

        void SetQuestFirst(BotGuid bot, bool on) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return;
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(p))
                botAI->SetRpgQuestFirst(on);
#else
            (void)bot; (void)on;
#endif
        }

        std::string GetRpgActivity(BotGuid bot) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return "offline";
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return "no-ai";
            char const* st;
            switch (botAI->rpgInfo.GetStatus())
            {
                case RPG_IDLE:          st = "idle"; break;
                case RPG_GO_GRIND:      st = "go-grind"; break;
                case RPG_GO_CAMP:       st = "go-camp"; break;
                case RPG_WANDER_RANDOM: st = "wander-random"; break;
                case RPG_WANDER_NPC:    st = "wander-npc"; break;
                case RPG_DO_QUEST:      st = "do-quest"; break;
                case RPG_TRAVEL_FLIGHT: st = "travel-flight"; break;
                case RPG_REST:          st = "rest"; break;
                case RPG_OUTDOOR_PVP:   st = "outdoor-pvp"; break;
                default:                st = "?"; break;
            }
            // diag: a=AllowActivity r=has "new rpg" strategy (why is she idle?)
            return std::string(st) + "[a=" + (botAI->AllowActivity() ? "1" : "0")
                 + " r=" + (botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT) ? "1" : "0") + "]";
#else
            (void)bot;
            return "no-playerbots";
#endif
        }

        // Tell the bot to attack a specific creature when provided; otherwise fall
        // back to playerbots' grind targeting.
        bool AttackCreature(BotGuid bot, uint64_t creatureGuid) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || p->IsInCombat())
                return false;
            if (creatureGuid)
            {
#ifdef MOD_PLAYERBOTS
                PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
                if (!botAI)
                    return false;

                ObjectGuid guid(creatureGuid);
                Unit* target = botAI->GetUnit(guid);
                if (!target || !target->IsInWorld() || target->isDead() ||
                    p->IsFriendlyTo(target) || !p->IsWithinLOSInMap(target) ||
                    !p->IsValidAttackTarget(target))
                    return false;

                AiObjectContext* context = botAI->GetAiObjectContext();
                context->GetValue<GuidVector>("prioritized targets")->Set({ guid });
                context->GetValue<ObjectGuid>("pull target")->Set(guid);
                context->GetValue<Unit*>("current target")->Set(target);
                context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);

                p->SetSelection(guid);

                if (p->isMoving() && p->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_CONTROLLED) == NULL_MOTION_TYPE)
                {
                    p->GetMotionMaster()->Clear(false);
                    p->StopMoving();
                }

                botAI->ChangeEngine(BOT_STATE_COMBAT);
                return p->Attack(target, p->IsWithinMeleeRange(target) || botAI->IsMelee(p));
#else
                return false;
#endif
            }
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

        bool FindNearestQuestCreature(BotGuid bot, std::vector<uint32_t> const& entries, BotPosition const& center, float radius, BotPosition& out, uint64_t& outGuid) override
        {
            outGuid = 0;
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;

            float searchRadius = radius;
            if (center.valid && center.mapId == p->GetMapId())
            {
                float const dx = p->GetPositionX() - center.x;
                float const dy = p->GetPositionY() - center.y;
                searchRadius += std::sqrt(dx * dx + dy * dy);
            }

            std::list<Creature*> creatures;
            p->GetCreatureListWithEntryInGrid(creatures, entries, searchRadius);

            Creature* best = nullptr;
            float bestDist = 0.f;
            for (Creature* c : creatures)
            {
                if (!c || !c->IsInWorld() || c->isDead() || !p->IsValidAttackTarget(c))
                    continue;

                if (center.valid)
                {
                    if (c->GetMapId() != center.mapId)
                        continue;

                    float const cx = c->GetPositionX() - center.x;
                    float const cy = c->GetPositionY() - center.y;
                    if ((cx * cx + cy * cy) > radius * radius)
                        continue;
                }

                float const d = p->GetDistance(c);
                if (!best || d < bestDist)
                {
                    best = c;
                    bestDist = d;
                }
            }
            if (!best)
                return false;
            outGuid = best->GetGUID().GetRawValue();
            out.mapId = best->GetMapId();
            out.x = best->GetPositionX();
            out.y = best->GetPositionY();
            out.z = best->GetPositionZ();
            out.valid = true;
            return true;
        }

        bool FindNearestHostile(BotGuid bot, float radius, BotPosition& out, uint64_t& outGuid) override
        {
            outGuid = 0;
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI || !botAI->GetAiObjectContext())
                return false;

            // "possible targets" is the playerbots-maintained list of attackable
            // mobs (already excludes tapped/claimed corpses), so we reuse it rather
            // than re-scanning the grid — keeps "what's around me" consistent with
            // what the class AI sees.
            AiObjectContext* ctx = botAI->GetAiObjectContext();
            auto* possible = ctx->GetValue<GuidVector>("possible targets");
            if (!possible)
                return false;

            Unit* best = nullptr;
            float bestDist = 0.f;
            for (ObjectGuid const& guid : possible->Get())
            {
                Unit* u = botAI->GetUnit(guid);
                if (!u || !u->IsInWorld() || u->isDead() ||
                    p->IsFriendlyTo(u) || !p->IsValidAttackTarget(u) ||
                    !p->IsWithinLOSInMap(u))
                    continue;

                float const d = p->GetDistance(u);
                if (d > radius)
                    continue;
                if (!best || d < bestDist)
                {
                    best = u;
                    bestDist = d;
                }
            }
            if (!best)
                return false;

            outGuid = best->GetGUID().GetRawValue();
            out.mapId = best->GetMapId();
            out.x = best->GetPositionX();
            out.y = best->GetPositionY();
            out.z = best->GetPositionZ();
            out.valid = true;
            return true;
#else
            (void)bot;
            (void)radius;
            (void)out;
            return false;
#endif
        }

        bool FindNearestServiceNpc(BotGuid bot, uint32_t npcFlagMask, float radius, BotPosition& out, uint64_t& outGuid) override
        {
            outGuid = 0;
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            // Collect every creature in range, then keep the nearest friendly,
            // alive one that offers the requested service (npcflag).
            struct AnyCreatureInRange
            {
                WorldObject const* o;
                float r;
                AnyCreatureInRange(WorldObject const* o_, float r_) : o(o_), r(r_) {}
                bool operator()(Unit* u) { return o->IsWithinDist(u, r, false); }
            } checkAll(p, radius);

            std::list<Creature*> creatures;
            Acore::CreatureListSearcher<AnyCreatureInRange> searcher(p, creatures, checkAll);
            Cell::VisitObjects(p, searcher, radius);

            Creature* best = nullptr;
            float bestDist = 0.f;
            for (Creature* c : creatures)
            {
                if (!c || !c->IsAlive() || c->IsInCombat())
                    continue;
                if (!c->HasNpcFlag(static_cast<NPCFlags>(npcFlagMask)))
                    continue;
                if (p->IsHostileTo(c) || !c->IsWithinLOSInMap(p))
                    continue;
                // For a class-trainer search, only accept a trainer that can
                // actually teach THIS bot (right class), not just any trainer.
                if (npcFlagMask & 0x20 /*UNIT_NPC_FLAG_TRAINER_CLASS*/)
                {
                    Trainer::Trainer* tr = sObjectMgr->GetTrainer(c->GetEntry());
                    if (!tr || !tr->IsTrainerValidForPlayer(p))
                        continue;
                }

                float const d = p->GetDistance(c);
                if (!best || d < bestDist)
                {
                    best = c;
                    bestDist = d;
                }
            }
            if (!best)
                return false;

            outGuid = best->GetGUID().GetRawValue();
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
            GameObject* go = p->FindNearestGameObject(entry, radius);
            return go ? go->GetGUID().GetRawValue() : 0;
        }

        bool IsNearGameObject(BotGuid bot, uint32_t entry, float radius) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            return p->FindNearestGameObject(entry, radius) != nullptr;
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
            GameObject* go = p->FindNearestGameObject(entry, radius);
            if (!go)
                return false;
            go->Use(p);
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': used gameobject entry {} ({}).",
                p->GetName(), entry, go->GetGUID().ToString());
            return true;
        }

        // --- maintenance (routed through playerbots actions) ---
        LootAttempt LootNearby(BotGuid bot) override
        {
            LootAttempt result;
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return result;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return result;

            AiObjectContext* context = botAI->GetAiObjectContext();
            LootObject loot = context->GetValue<LootObject>("loot target")->Get();
            if (loot.IsEmpty() || !loot.IsLootPossible(p))
                loot = context->GetValue<LootObjectStack*>("available loot")->Get()->GetLoot(IdleBotLootSearchRadius);

            if (loot.IsEmpty())
                return result;

            WorldObject* lootObject = loot.GetWorldObject(p);
            if (!lootObject)
                return result;

            if (Creature* creature = lootObject->ToCreature())
            {
                result.corpseEntry = creature->GetEntry();
                result.corpseGuid = creature->GetGUID().GetRawValue();
                DescribeCreatureLoot(p, botAI, creature, result);
            }

            if (p->GetDistance(lootObject) > INTERACTION_DISTANCE - 2.0f)
            {
                result.hasLoot = true;
                result.inRange = false;
                return result;
            }

            result.hasLoot = true;
            result.inRange = true;
            return result;
#else
            (void)bot;
            return result;
#endif
        }
        bool VendorTrash(BotGuid bot) override { return DoBotAction(bot, "sell"); }
        bool Repair(BotGuid bot) override      { return DoBotAction(bot, "repair"); }
        bool Train(BotGuid bot) override       { return DoBotAction(bot, "trainer"); }

        bool LearnAvailableSpells(BotGuid bot) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            // The factory's own spell init: InitAvailableSpells iterates every class
            // trainer and learns each spell the bot is level-eligible for (the same
            // path that fully-spells randomized bots) — reliable and complete,
            // unlike the interactive "trainer" chat action. We make the bot travel to
            // a real trainer first (player-like), then learn here.
            uint32 const before = uint32(p->GetSpellMap().size());
            PlayerbotFactory factory(p, p->GetLevel());
            factory.InitClassSpells();
            factory.InitAvailableSpells();
            uint32 const after = uint32(p->GetSpellMap().size());
            p->SaveToDB(false, false);   // persist immediately (verifiable, crash-safe)
            LOG_INFO("module.idlebot",
                "[IdleBot] LearnAvailableSpells '{}' L{}: spells {} -> {}",
                p->GetName(), p->GetLevel(), before, after);
            return true;
#else
            (void)bot;
            return false;
#endif
        }
        bool Maintenance(BotGuid bot) override { return DoBotAction(bot, "maintenance"); }

        bool AutoSpecTalents(BotGuid bot) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            // Matches the factory's own spec call: reset + template-allocate for the
            // class/level. Re-applying each level keeps a coherent leveling build.
            uint32 const before = p->GetFreeTalentPoints();
            PlayerbotFactory factory(p, p->GetLevel());
            factory.InitTalentsTree(true /*incremental*/, true /*use_template*/, true /*reset*/);
            uint32 const after = p->GetFreeTalentPoints();
            // Persist immediately so a crash before the next PlayerSave can't lose the
            // build, and so it's externally verifiable.
            p->SaveToDB(false, false);
            LOG_INFO("module.idlebot",
                "[IdleBot] AutoSpecTalents '{}' L{}: freeTalentPoints {} -> {}",
                p->GetName(), p->GetLevel(), before, after);
            return true;
#else
            (void)bot;
            return false;
#endif
        }

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
                if (auto* possibleTargets = ctx->GetValue<GuidVector>("possible targets"))
                    out.possibleTargets = static_cast<uint32_t>(possibleTargets->Get().size());
                if (auto* myAttackers = ctx->GetValue<uint8>("my attackers count"))
                    out.myAttackers = myAttackers->Get();
                if (auto* aoeCount = ctx->GetValue<uint8>("aoe count"))
                    out.aoeCount = aoeCount->Get();
                if (auto* currentTarget = ctx->GetValue<Unit*>("current target"))
                {
                    Unit* target = currentTarget->Get();
                    if (target && target->IsInWorld())
                    {
                        out.currentTargetEntry = target->GetEntry();
                        out.currentTargetDistance = p->GetDistance(target);
                        out.currentTargetName = target->GetName();
                    }
                }
            }
            if (!out.currentTargetEntry)
            {
                if (Unit* target = p->GetSelectedUnit())
                {
                    if (target->IsInWorld())
                    {
                        out.currentTargetEntry = target->GetEntry();
                        out.currentTargetDistance = p->GetDistance(target);
                        out.currentTargetName = target->GetName();
                    }
                }
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

        uint32_t GetItemCount(BotGuid bot, uint32_t itemId, bool inBankAlso) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p ? p->GetItemCount(itemId, inBankAlso) : 0;
        }

        bool GetQuestObjectiveProgress(BotGuid bot, uint32_t questId, uint8_t objectiveIndex, uint32_t& outCurrent, uint32_t& outRequired) override
        {
            outCurrent = 0;
            outRequired = 0;

            Player* p = ResolveOnlinePlayer(bot);
            if (!p || objectiveIndex >= QUEST_OBJECTIVES_COUNT)
                return false;

            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                return false;

            if (quest->RequiredNpcOrGo[objectiveIndex] != 0)
            {
                int32_t const entry = quest->RequiredNpcOrGo[objectiveIndex];
                outRequired = quest->RequiredNpcOrGoCount[objectiveIndex];
                outCurrent = p->GetReqKillOrCastCurrentCount(questId, entry);

                uint16 const slot = p->FindQuestSlot(questId);
                if (slot < MAX_QUEST_LOG_SIZE)
                    outCurrent = std::max<uint32_t>(outCurrent, p->GetQuestSlotCounter(slot, objectiveIndex));

                return outRequired > 0;
            }

            if (quest->RequiredItemId[objectiveIndex] != 0)
            {
                outRequired = quest->RequiredItemCount[objectiveIndex];
                outCurrent = p->GetItemCount(quest->RequiredItemId[objectiveIndex], false);

                auto const qsIt = p->getQuestStatusMap().find(questId);
                if (qsIt != p->getQuestStatusMap().end())
                    outCurrent = std::max<uint32_t>(outCurrent, qsIt->second.ItemCount[objectiveIndex]);

                return outRequired > 0;
            }

            return false;
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
