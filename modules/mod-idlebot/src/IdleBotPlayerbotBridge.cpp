#include "IdleBotPlayerbotBridge.h"
#include "CharacterCache.h"
#include "ObjectAccessor.h"
#include "Player.h"
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

        QuestState GetQuestStatus(BotGuid /*bot*/, uint32_t /*questId*/) override
        {
            return QuestState::Unknown;  // TODO(M3): map Player quest status
        }

        // --- actions: deferred to the executor milestones (M3+) ---
        bool MoveTo(BotGuid, uint32_t, float, float, float, float) override { return false; }
        bool FollowPlayer(BotGuid, PlayerGuid) override { return false; }
        bool AttackCreature(BotGuid, uint64_t) override { return false; }
        bool CastSpell(BotGuid, uint32_t, uint64_t) override { return false; }
        bool InteractWithNpc(BotGuid, uint64_t) override { return false; }
        bool AcceptQuest(BotGuid, uint32_t, uint64_t) override { return false; }
        bool TurnInQuest(BotGuid, uint32_t, uint64_t) override { return false; }
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
