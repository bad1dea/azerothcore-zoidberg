#include "IdleBotPlayerbotBridge.h"
#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Creature.h"
#include "ObjectMgr.h"
#include "QuestDef.h"
#include "MotionMaster.h"
#include "Log.h"

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
#include "Bot/PlayerbotAI.h"      // PlayerbotAI, DoSpecificAction
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
            // Master-less add; login is async (query holder + world callback).
            sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
            LOG_INFO("module.idlebot", "[IdleBot] queued master-less login for '{}'.", botName);
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

        InventoryStatus GetInventoryStatus(BotGuid /*bot*/) override
        {
            return InventoryStatus{};   // TODO(M4): populate free/total slots, repair need
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

        // --- actions deferred to M4+ ---
        bool FollowPlayer(BotGuid, PlayerGuid) override { return false; }

        // Tell the bot to attack the nearest viable mob via its own grind targeting.
        // creatureGuid is ignored (0 = pick nearest); the bot AI's GrindTargetValue
        // handles quest-need prioritisation and level/range checks.
        bool AttackCreature(BotGuid bot, uint64_t /*creatureGuid*/) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolvePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat())
                return false;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return false;

            // "attack anything" → AttackAnythingAction → GrindTargetValue picks the
            // nearest hostile mob (quest-needed mobs prioritised). Silent=true so the
            // bot doesn't emote on every tick.
            return botAI->DoSpecificAction("attack anything", Event(), true /*silent*/);
#else
            return false;
#endif
        }
        bool CastSpell(BotGuid, uint32_t, uint64_t) override { return false; }
        bool LootNearby(BotGuid) override { return false; }
        bool VendorTrash(BotGuid) override { return false; }
        bool Repair(BotGuid) override { return false; }
        bool Train(BotGuid) override { return false; }
        bool ReviveOrCorpseRun(BotGuid) override { return false; }

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
