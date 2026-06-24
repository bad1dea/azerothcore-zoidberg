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
#include "SpellInfo.h"             // TARGET_FLAG_UNIT
#include "MotionMaster.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Transport.h"
#include "Map.h"
#include "DBCStores.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "SpellAuraEffects.h"
#include "ItemPackets.h"

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
            // "controlled" REQUIRES a live PlayerbotAI. A bot can be connected and in
            // the playerbots map yet have no AI (a logout/release that left a stale map
            // entry) — the "online, no AI" wedge. Stock OnBotLogin early-returns when the
            // guid is already mapped, so it can't rebuild the AI. Clear the stale map
            // entry first (public RemoveFromPlayerbotsMap), then OnBotLogin recreates the
            // AI in-place. This is the in-module replacement for the former playerbots
            // OnBotLogin fork patch — it uses only stock public API, so playerbots stays
            // unmodified.
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
            {
                sRandomPlayerbotMgr.RemoveFromPlayerbotsMap(p->GetGUID());
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

            if (p->IsInCombat())
            {
                if (p->IsMounted())
                    p->RemoveAurasByType(SPELL_AURA_MOUNTED);
                return true;
            }

            p->UpdateAllowedPositionZ(x, y, z);

            float const dx = x - p->GetPositionX();
            float const dy = y - p->GetPositionY();
            float const distSq = dx * dx + dy * dy;
            float constexpr MaxSegment = 200.f;
            float constexpr MountDistance = 75.f * 75.f;

            // Mount for long distances, dismount when close or in water.
            bool const inWater = p->IsInWater() || p->IsUnderWater();
            if (p->GetLevel() >= 20 && distSq > MountDistance && !p->IsMounted() &&
                !p->GetTransport() && !inWater)
                DoBotAction(bot, "mount");
            else if (p->IsMounted() && (distSq < 30.f * 30.f || inWater))
                p->RemoveAurasByType(SPELL_AURA_MOUNTED);

            // Long-distance: move toward an intermediate point ~200yd along the
            // line to the destination. Each tick advances one segment; the navmesh
            // handles each segment cleanly (74 points × 4yd = 296yd max per query).
            float moveX = x, moveY = y, moveZ = z;
            if (distSq > MaxSegment * MaxSegment)
            {
                float const dist = std::sqrt(distSq);
                float const ratio = MaxSegment / dist;
                moveX = p->GetPositionX() + dx * ratio;
                moveY = p->GetPositionY() + dy * ratio;
                moveZ = p->GetPositionZ();
                p->UpdateAllowedPositionZ(moveX, moveY, moveZ);
            }

            p->GetMotionMaster()->MovePoint(0, moveX, moveY, moveZ, FORCED_MOVEMENT_NONE, 0.f, 0.f,
                                            true /*generatePath*/, false /*forceDestination*/);
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

        static uint8 BestArmorSubclass(Player const* p)
        {
            switch (p->getClass())
            {
                case CLASS_WARRIOR:
                case CLASS_PALADIN:
                    return p->GetLevel() >= 40 ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
                case CLASS_HUNTER:
                case CLASS_SHAMAN:
                    return p->GetLevel() >= 40 ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
                case CLASS_ROGUE:
                case CLASS_DRUID:
                    return ITEM_SUBCLASS_ARMOR_LEATHER;
                case CLASS_DEATH_KNIGHT:
                    return ITEM_SUBCLASS_ARMOR_PLATE;
                default:
                    return ITEM_SUBCLASS_ARMOR_CLOTH;
            }
        }

        static float StatWeight(uint8 cls, uint32 statType)
        {
            switch (statType)
            {
                case ITEM_MOD_STAMINA: return 1.0f;
                case ITEM_MOD_STRENGTH:
                    return (cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_DEATH_KNIGHT) ? 2.0f : 0.5f;
                case ITEM_MOD_AGILITY:
                    return (cls == CLASS_HUNTER || cls == CLASS_ROGUE || cls == CLASS_DRUID) ? 2.0f : 0.5f;
                case ITEM_MOD_INTELLECT:
                    return (cls == CLASS_MAGE || cls == CLASS_WARLOCK || cls == CLASS_PRIEST ||
                            cls == CLASS_SHAMAN || cls == CLASS_DRUID || cls == CLASS_PALADIN) ? 2.0f : 0.1f;
                case ITEM_MOD_SPIRIT:
                    return (cls == CLASS_MAGE || cls == CLASS_WARLOCK || cls == CLASS_PRIEST) ? 1.0f : 0.2f;
                case ITEM_MOD_SPELL_POWER:
                    return (cls == CLASS_MAGE || cls == CLASS_WARLOCK || cls == CLASS_PRIEST ||
                            cls == CLASS_SHAMAN || cls == CLASS_DRUID || cls == CLASS_PALADIN) ? 2.2f : 0.0f;
                case ITEM_MOD_ATTACK_POWER:
                    return (cls == CLASS_WARRIOR || cls == CLASS_ROGUE || cls == CLASS_HUNTER) ? 1.5f : 0.0f;
                case ITEM_MOD_HIT_RATING:
                case ITEM_MOD_CRIT_RATING:
                    return 1.5f;
                default: return 0.5f;
            }
        }

        static uint32_t ChooseBestReward(Player const* p, Quest const* quest)
        {
            uint32_t bestIdx = 0;
            float bestScore = -1.f;
            uint8 const idealArmor = BestArmorSubclass(p);
            uint8 const cls = p->getClass();

            for (uint32_t i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
            {
                uint32_t itemId = quest->RewardChoiceItemId[i];
                if (!itemId)
                    continue;

                ItemTemplate const* it = sObjectMgr->GetItemTemplate(itemId);
                if (!it)
                    continue;

                float score = static_cast<float>(it->ItemLevel);

                if (p->CanUseItem(it) == EQUIP_ERR_OK)
                    score += 1000.f;

                // Armor type bonus
                if (it->Class == ITEM_CLASS_ARMOR && it->SubClass > ITEM_SUBCLASS_ARMOR_MISC)
                {
                    if (it->SubClass == idealArmor)
                        score += 500.f;
                    else if (it->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD &&
                             (cls == CLASS_WARRIOR || cls == CLASS_PALADIN || cls == CLASS_SHAMAN))
                        score += 400.f;
                }

                // Weapon DPS scoring
                if (it->Class == ITEM_CLASS_WEAPON && it->Delay > 0)
                {
                    float dps = 0.f;
                    for (auto const& dmg : it->Damage)
                        dps += (dmg.DamageMin + dmg.DamageMax) / 2.f;
                    dps = dps * 1000.f / static_cast<float>(it->Delay);
                    score += dps * 3.f;
                }

                // Stat weight scoring
                for (uint32_t s = 0; s < it->StatsCount && s < MAX_ITEM_PROTO_STATS; ++s)
                {
                    float w = StatWeight(cls, it->ItemStat[s].ItemStatType);
                    score += static_cast<float>(it->ItemStat[s].ItemStatValue) * w;
                }

                if (score > bestScore)
                {
                    bestScore = score;
                    bestIdx = i;
                }
            }
            return bestIdx;
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

            uint32_t const rewardIdx = ChooseBestReward(p, quest);

            if (npcEntry32 == 0)
            {
                p->RewardQuest(quest, rewardIdx, nullptr, true /*announce*/);
                if (p->GetQuestStatus(questId) == QUEST_STATUS_REWARDED)
                    p->SaveToDB(false, false);
                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': turned in quest {} (reward {}) without explicit questgiver.",
                    p->GetName(), questId, rewardIdx);
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
            packet << npc->GetGUID() << questId << rewardIdx;
            packet.rpos(0);
            p->GetSession()->HandleQuestgiverChooseRewardOpcode(packet);
            if (p->GetQuestStatus(questId) == QUEST_STATUS_REWARDED)
                p->SaveToDB(false, false);

            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': turned in quest {} (reward {}).", p->GetName(), questId, rewardIdx);
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

        // Self-sufficiency (2026-06-22): mod-idlebot must build against STOCK core and
        // STOCK mod-playerbots — no out-of-module patches. The former force-active hook
        // (custom PlayerbotAI::SetForceActive) is therefore gone. We instead rely on
        // playerbots' stock activity rules in PlayerbotAI::AllowActive: a bot in combat
        // is always active, and the global AiPlayerbot.BotActiveAlone knob governs
        // alone-activity. idlebot's own per-tick MoveTo/AttackCreature keep the bot
        // engaged, so combat (the bulk of leveling) keeps it active without the hook.
        // This also lets us drop the core use-after-free patch that force-active combat
        // required. Kept as a no-op so the manager call site stays stable.
        void SetForceActive(BotGuid bot, bool on) override
        {
            (void)bot; (void)on;
        }

        // The former quest-first hook (custom PlayerbotAI::SetRpgQuestFirst) biased the
        // autonomous "new rpg" weights toward questing; it only affected ORGANIC mode
        // (strict mode drives explicit guide steps and never consulted it). Dropped for
        // stock-playerbots compatibility — organic mode falls back to stock RPG weights
        // (tunable via AiPlayerbot.RpgStatusProbWeight* if desired). No-op.
        void SetQuestFirst(BotGuid bot, bool on) override
        {
            (void)bot; (void)on;
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
                bool const inMelee = p->IsWithinMeleeRange(target);
                return p->Attack(target, inMelee);
#else
                return false;
#endif
            }
            // "attack anything" → AttackAnythingAction → GrindTargetValue picks the
            // nearest hostile mob (quest-needed mobs prioritised).
            return DoBotAction(bot, "attack anything");
        }

        // Re-target mid-combat (AttackCreature refuses while in combat). Same
        // target-set path, used to clear adds that are beating on the bot while the
        // class AI tunnels a distant mob. Requires a specific creature; no fallback.
        bool SwitchTarget(BotGuid bot, uint64_t creatureGuid) override
        {
            if (!creatureGuid)
                return false;
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return false;

            ObjectGuid guid(creatureGuid);
            Unit* target = botAI->GetUnit(guid);
            if (!target || !target->IsInWorld() || target->isDead() ||
                p->IsFriendlyTo(target) || !p->IsWithinLOSInMap(target) ||
                !p->IsValidAttackTarget(target))
                return false;

            // Don't bother re-asserting if it's already the current target.
            if (p->GetTarget() == guid)
                return true;

            AiObjectContext* context = botAI->GetAiObjectContext();
            context->GetValue<GuidVector>("prioritized targets")->Set({ guid });
            context->GetValue<ObjectGuid>("pull target")->Set(guid);
            context->GetValue<Unit*>("current target")->Set(target);

            p->SetSelection(guid);
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            return p->Attack(target, p->IsWithinMeleeRange(target) || botAI->IsMelee(p));
#else
            (void)bot;
            return false;
#endif
        }

        // Use an inventory item ON a hostile/neutral unit (CAST-flagged quests:
        // SpecialFlags=32, where the objective is "use the quest item on creature X"
        // — e.g. q5441 wake a Lazy Peon with a horn). Playerbots' own UseItemAction
        // only ever uses an item with no unit target (UseItemAuto) unless an active
        // player master has the unit selected; a master-less idlebot has no such
        // path, so we drive the use directly. Replicates the unit-target branch of
        // UseItemAction::UseItem (CMSG_USE_ITEM + TARGET_FLAG_UNIT) and dispatches
        // through the normal session handler — the same pattern playerbots uses for
        // AcceptQuest. Verified against WorldSession::HandleUseItemOpcode packet read
        // order in src/server/game/Handlers/SpellHandler.cpp:58.
        bool UseItemOnTarget(BotGuid bot, uint32_t itemId, uint64_t targetGuid) override
        {
            if (!itemId || !targetGuid)
                return false;
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(p);
            if (!botAI)
                return false;

            Item* item = p->GetItemByEntry(itemId);
            if (!item || p->CanUseItem(item) != EQUIP_ERR_OK || p->IsNonMeleeSpellCast(false))
                return false;

            ObjectGuid guid(targetGuid);
            Unit* target = botAI->GetUnit(guid);
            if (!target || !target->IsInWorld() || target->isDead())
                return false;

            // Resolve the item's on-use spell and confirm the bot can cast it.
            uint32 spellId = 0;
            ItemTemplate const* proto = item->GetTemplate();
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                if (proto->Spells[i].SpellId > 0)
                {
                    spellId = proto->Spells[i].SpellId;
                    break;
                }
            }
            if (spellId == 0 || !botAI->CanCastSpell(spellId, target, false))
                return false;

            uint8 const bagIndex = item->GetBagSlot();
            uint8 const slot = item->GetSlot();
            uint8 const castCount = 1;
            uint32 const glyphIndex = 0;
            uint8 const castFlags = 0;
            uint32 const targetFlag = TARGET_FLAG_UNIT;

            WorldPacket packet(CMSG_USE_ITEM);
            packet << bagIndex << slot << castCount << spellId << item->GetGUID()
                   << glyphIndex << castFlags;
            packet << targetFlag;
            packet << guid.WriteAsPacked();

            // Face the target so the cast isn't rejected for orientation.
            p->SetFacingToObject(target);
            p->SetSelection(guid);
            p->GetSession()->HandleUseItemOpcode(packet);
            return true;
#else
            (void)bot; (void)itemId; (void)targetGuid;
            return false;
#endif
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
            if (!go || !go->isSpawned())
                return false;

            // For chest-type GOs (type 3), send the proper loot packet.
            // The loot is generated by the handler; on the NEXT tick the
            // playerbots +loot strategy picks up the items. We also try
            // immediate auto-store as a fast path.
            if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST)
            {
                WorldPacket pkt(CMSG_GAMEOBJ_USE, 8);
                pkt << go->GetGUID();
                pkt.rpos(0);
                p->GetSession()->HandleGameObjectUseOpcode(pkt);

                // Also try the playerbots loot action — it handles GO loot pickup.
                DoBotAction(bot, "loot");

                // Auto-store loot items from the GO.
                if (p->GetLootGUID() == go->GetGUID())
                {
                    Loot* loot = &go->loot;
                    uint32 maxSlot = loot->GetMaxSlotInLootFor(p);
                    for (uint32 i = 0; i < maxSlot; ++i)
                    {
                        LootItem* item = loot->LootItemInSlot(i, p);
                        if (!item || item->is_looted)
                            continue;

                        // Auto-store into bags.
                        ItemPosCountVec dest;
                        InventoryResult res = p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest,
                            item->itemid, item->count);
                        if (res == EQUIP_ERR_OK)
                        {
                            Item* newItem = p->StoreNewItem(dest, item->itemid, true,
                                item->randomPropertyId);
                            if (newItem)
                            {
                                p->SendNewItem(newItem, item->count, false, false, true);
                                item->is_looted = true;
                                --loot->unlootedCount;
                                LOG_INFO("module.idlebot",
                                    "[IdleBot] bot '{}': looted item {} x{} from GO {}.",
                                    p->GetName(), item->itemid, uint32(item->count), entry);
                            }
                        }
                    }

                    // Release the loot.
                    p->GetSession()->DoLootRelease(go->GetGUID());
                }
            }
            else
            {
                go->Use(p);
            }

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

        bool EnsureStarterGear(BotGuid bot) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotFactory factory(p, p->GetLevel());
            factory.InitClassSpells();
            factory.InitAvailableSpells();
            // L<=5: non-incremental to fill empty slots with starting gear.
            // L>5: incremental to upgrade without filling bags with junk.
            factory.InitEquipment(p->GetLevel() > 5);
            factory.InitAmmo();
            p->SaveToDB(false, false);   // persist immediately (verifiable, crash-safe)
            LOG_INFO("module.idlebot",
                "[IdleBot] EnsureStarterGear '{}' L{}: equipped + spelled for combat.",
                p->GetName(), p->GetLevel());
            return true;
#else
            (void)bot;
            return false;
#endif
        }

        // Equip bags so the bot has room for loot AND quest rewards. The default
        // 16-slot backpack fills with loot; once near-full, a quest TURN-IN whose
        // reward can't fit SILENTLY fails server-side (CanRewardQuest with the reward
        // index fails for lack of space) and the bot loops forever at the ender — a
        // real, observed leveling stall. InitBags(false) is NON-destructive: it only
        // fills EMPTY bag slots (keeps any bags the bot earned), using the factory's
        // standard bag (item 51809, 24 slots) — 4 slots => +96 slots.
        bool EnsureBags(BotGuid bot) override
        {
#ifdef MOD_PLAYERBOTS
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            PlayerbotFactory factory(p, p->GetLevel());
            factory.InitBags(false /*don't destroy earned bags*/);
            p->SaveToDB(false, false);
            LOG_INFO("module.idlebot", "[IdleBot] bot '{}': ensured bags (free slots now {}).",
                p->GetName(), p->GetFreeInventorySpace());
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

        bool FollowCreature(BotGuid bot, uint64_t creatureGuid, float distance) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;
            Creature* c = ObjectAccessor::GetCreature(*p, ObjectGuid(creatureGuid));
            if (!c || !c->IsAlive())
                return false;
            float dx = p->GetPositionX() - c->GetPositionX();
            float dy = p->GetPositionY() - c->GetPositionY();
            if ((dx*dx + dy*dy) > distance * distance)
                p->GetMotionMaster()->MovePoint(0, c->GetPositionX(), c->GetPositionY(),
                    c->GetPositionZ(), FORCED_MOVEMENT_NONE, 0.f, 0.f, true, false);
            return true;
        }

        bool TaxiTo(BotGuid bot, uint32_t taxiNodeId) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;
            // Server-side: just teleport to the taxi node destination.
            // Full taxi-ride animation is client-only and not worth simulating.
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(taxiNodeId);
            if (!node)
                return false;
            p->TeleportTo(node->map_id, node->x, node->y, node->z + 2.f, p->GetOrientation());
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': taxi to node {} (map {} {:.0f},{:.0f}).",
                p->GetName(), taxiNodeId, node->map_id, node->x, node->y);
            return true;
        }

        bool GossipSelect(BotGuid bot, uint64_t npcGuid, uint32_t optionIndex) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;
            WorldPacket packet(CMSG_GOSSIP_SELECT_OPTION);
            packet << ObjectGuid(npcGuid) << uint32_t(0) << optionIndex;
            packet.rpos(0);
            p->GetSession()->HandleGossipSelectOptionOpcode(packet);
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': gossip select option {} on npc.",
                p->GetName(), optionIndex);
            return true;
        }

        bool UseHearthstone(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat())
                return false;
            Item* hs = p->GetItemByEntry(6948);
            if (!hs)
                return false;
            SpellCastTargets targets;
            targets.SetUnitTarget(p);
            p->CastItemUseSpell(hs, targets, 0, 0);
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': using hearthstone.", p->GetName());
            return true;
        }

        bool HasHearthstone(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p && p->HasItemCount(6948, 1);
        }

        bool IsHearthstoneReady(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            Item* hs = p->GetItemByEntry(6948);
            if (!hs)
                return false;
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(8690);
            if (!spell)
                return false;
            return !p->HasSpellCooldown(8690);
        }

        bool HasResSickness(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p && p->HasAura(15007);
        }

        bool HasSoulstone(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p) return false;
            return p->HasAura(20707) || p->HasAura(20762) || p->HasAura(20763)
                || p->HasAura(20764) || p->HasAura(20765);
        }

        bool IsDungeon(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p && p->GetMap() && p->GetMap()->IsDungeon();
        }

        bool HasNearbyRealPlayer(BotGuid bot, float /*range*/) override
        {
            // Stub — would need grid-search for non-bot players.
            // Non-compete distance (#56) is low-priority for private servers.
            (void)bot;
            return false;
        }

        bool UseItem(BotGuid bot, uint32_t itemId) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat())
                return false;
            Item* item = p->GetItemByEntry(itemId);
            if (!item)
                return false;
            SpellCastTargets targets;
            targets.SetUnitTarget(p);
            p->CastItemUseSpell(item, targets, 0, 0);
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': used item {} (entry {}).",
                p->GetName(), item->GetTemplate()->Name1, itemId);
            return true;
        }

        uint32_t SellByQuality(BotGuid bot, uint32_t maxQuality) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return 0;

            // Find nearby vendor — can't use entry=0 grid search (returns empty).
            // Use FindNearestCreatureWithOptions or iterate VisibleCreatures.
            BotPosition vendorPos;
            uint64_t vendorGuid = 0;
            if (!FindNearestServiceNpc(bot, UNIT_NPC_FLAG_VENDOR, 10.f, vendorPos, vendorGuid) || vendorGuid == 0)
                return 0;
            Creature* vendor = ObjectAccessor::GetCreature(*p, ObjectGuid(vendorGuid));
            if (!vendor)
                return 0;

            uint32_t sold = 0;
            for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            {
                if (Bag* pBag = p->GetBagByPos(bag))
                {
                    for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                    {
                        Item* item = p->GetItemByPos(bag, slot);
                        if (!item) continue;
                        ItemTemplate const* proto = item->GetTemplate();
                        if (!proto) continue;
                        if (proto->Quality > maxQuality) continue;
                        if (proto->Class == ITEM_CLASS_QUEST) continue;
                        if (proto->Bonding == BIND_QUEST_ITEM) continue;
                        if (proto->StartQuest > 0) continue;
                        if (proto->SellPrice == 0) continue;

                                WorldPacket pkt(CMSG_SELL_ITEM, 8 + 8 + 1);
                        pkt << vendor->GetGUID() << item->GetGUID() << uint8(item->GetCount());
                        pkt.rpos(0);
                        WorldPackets::Item::SellItem sellPkt(std::move(pkt));
                        sellPkt.Read();
                        p->GetSession()->HandleSellItemOpcode(sellPkt);
                        ++sold;
                    }
                }
            }
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            {
                Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
                if (!item) continue;
                ItemTemplate const* proto = item->GetTemplate();
                if (!proto) continue;
                if (proto->Quality > maxQuality) continue;
                if (proto->Class == ITEM_CLASS_QUEST) continue;
                if (proto->Bonding == BIND_QUEST_ITEM) continue;
                if (proto->StartQuest > 0) continue;
                if (proto->SellPrice == 0) continue;

                WorldPacket pkt(CMSG_SELL_ITEM, 8 + 8 + 1);
                pkt << vendor->GetGUID() << item->GetGUID() << uint8(item->GetCount());
                pkt.rpos(0);
                WorldPackets::Item::SellItem sellPkt(std::move(pkt));
                sellPkt.Read();
                p->GetSession()->HandleSellItemOpcode(sellPkt);
                ++sold;
            }
            if (sold > 0)
                p->SaveToDB(false, false);
            return sold;
        }

        uint32_t GetCreatureLevel(BotGuid bot, uint64_t creatureGuid) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p) return 0;
            Creature* c = ObjectAccessor::GetCreature(*p, ObjectGuid(creatureGuid));
            return c ? c->GetLevel() : 0;
        }

        bool SummonMount(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat() || p->IsMounted())
                return false;
            if (p->GetLevel() < 20)
                return false;
            return DoBotAction(bot, "mount");
        }

        bool Dismount(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsMounted())
                return false;
            p->RemoveAurasByType(SPELL_AURA_MOUNTED);
            return true;
        }

        bool IsMounted(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p && p->IsMounted();
        }

        bool ScanSafeReviveSpot(BotGuid bot, float radius, float& outX, float& outY, float& outZ) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            float bestDist = 0.f;
            bool found = false;
            float corpseX = p->GetPositionX(), corpseY = p->GetPositionY(), corpseZ = p->GetPositionZ();

            // Scan 24 points in a circle, pick the one furthest from hostiles.
            for (float angle = 0.f; angle < 2.f * M_PI; angle += M_PI / 12.f)
            {
                float testX = corpseX + std::cos(angle) * radius;
                float testY = corpseY + std::sin(angle) * radius;
                float testZ = corpseZ;
                p->UpdateAllowedPositionZ(testX, testY, testZ);

                // Check distance from this test point to the nearest hostile.
                // Use the bridge's FindNearestHostile from the bot's current pos
                // as an approximation — the hostile set doesn't change per test point.
                BotPosition hPos;
                uint64_t hGuid = 0;
                float minHostileDist = 999.f;
                if (FindNearestHostile(bot, radius * 2.f, hPos, hGuid) && hPos.valid)
                {
                    float hdx = testX - hPos.x, hdy = testY - hPos.y;
                    minHostileDist = std::sqrt(hdx * hdx + hdy * hdy);
                }

                if (minHostileDist > bestDist)
                {
                    bestDist = minHostileDist;
                    outX = testX;
                    outY = testY;
                    outZ = testZ;
                    found = true;
                }
            }
            return found && bestDist > 15.f;
        }

        bool BuyFood(BotGuid bot) override
        {
            return DoBotAction(bot, "buy");
        }

        bool IsCreatureTappedByOther(BotGuid bot, uint64_t creatureGuid) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p)
                return false;
            Creature* c = ObjectAccessor::GetCreature(*p, ObjectGuid(creatureGuid));
            if (!c)
                return false;
            return c->HasDynamicFlag(UNIT_DYNFLAG_TAPPED) && !c->isTappedBy(p);
        }

        bool FireAreaTrigger(BotGuid bot, uint32_t triggerId) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;
            WorldPacket packet(CMSG_AREATRIGGER, 4);
            packet << triggerId;
            packet.rpos(0);
            p->GetSession()->HandleAreaTriggerOpcode(packet);
            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': fired areatrigger {}.",
                p->GetName(), triggerId);
            return true;
        }

        bool JumpForward(BotGuid bot, float distance) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat())
                return false;
            float o = p->GetOrientation();
            float x = p->GetPositionX() + std::cos(o) * distance;
            float y = p->GetPositionY() + std::sin(o) * distance;
            float z = p->GetPositionZ();
            p->UpdateAllowedPositionZ(x, y, z);
            p->GetMotionMaster()->MoveJump(x, y, z + 1.f, 7.f, 5.f);
            return true;
        }

        bool StrafeMove(BotGuid bot, bool left, float distance) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat())
                return false;
            float o = p->GetOrientation() + (left ? M_PI_2 : -M_PI_2);
            float x = p->GetPositionX() + std::cos(o) * distance;
            float y = p->GetPositionY() + std::sin(o) * distance;
            float z = p->GetPositionZ();
            p->UpdateAllowedPositionZ(x, y, z);
            p->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.f, 0.f,
                                            true, false);
            return true;
        }

        bool MoveBackward(BotGuid bot, float distance) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->IsInCombat())
                return false;
            float o = p->GetOrientation() + M_PI;
            float x = p->GetPositionX() + std::cos(o) * distance;
            float y = p->GetPositionY() + std::sin(o) * distance;
            float z = p->GetPositionZ();
            p->UpdateAllowedPositionZ(x, y, z);
            p->GetMotionMaster()->MovePoint(0, x, y, z, FORCED_MOVEMENT_NONE, 0.f, 0.f,
                                            true, false);
            return true;
        }

        bool BoardTransport(BotGuid bot, uint32_t transportEntry) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld() || p->GetTransport())
                return false;

            Map* map = p->GetMap();
            if (!map)
                return false;

            for (Transport* t : map->GetAllTransports())
            {
                if (t->GetEntry() != transportEntry)
                    continue;

                float px = p->GetPositionX(), py = p->GetPositionY(), pz = p->GetPositionZ();
                float tx = t->GetPositionX(), ty = t->GetPositionY(), tz = t->GetPositionZ();
                float dx = px - tx, dy = py - ty;
                if ((dx * dx + dy * dy) > 80.f * 80.f)
                    continue;

                float ox = px, oy = py, oz = pz, oo = p->GetOrientation();
                t->CalculatePassengerOffset(ox, oy, oz, &oo);

                p->m_movementInfo.transport.guid = t->GetGUID();
                p->m_movementInfo.transport.pos.Relocate(ox, oy, oz, oo);
                p->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
                p->SetTransport(t);
                t->AddPassenger(p, false);

                LOG_INFO("module.idlebot",
                    "[IdleBot] bot '{}': boarded transport {} (entry {}).",
                    p->GetName(), t->GetName(), transportEntry);
                return true;
            }
            return false;
        }

        bool DisembarkTransport(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            Transport* t = p->GetTransport();
            if (!t)
                return false;

            t->RemovePassenger(p, false);
            p->SetTransport(nullptr);
            p->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
            p->m_movementInfo.transport.Reset();

            LOG_INFO("module.idlebot",
                "[IdleBot] bot '{}': disembarked transport {}.",
                p->GetName(), t->GetName());
            return true;
        }

        bool IsOnTransport(BotGuid bot) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            return p && p->GetTransport() != nullptr;
        }

        bool IsTransportStopped(BotGuid bot, uint32_t transportEntry,
            float dockX, float dockY, float dockZ, float range) override
        {
            Player* p = ResolveOnlinePlayer(bot);
            if (!p || !p->IsInWorld())
                return false;

            Map* map = p->GetMap();
            if (!map)
                return false;

            for (Transport* t : map->GetAllTransports())
            {
                if (t->GetEntry() != transportEntry)
                    continue;

                float tx = t->GetPositionX(), ty = t->GetPositionY();
                float dx = tx - dockX, dy = ty - dockY;
                return (dx * dx + dy * dy) <= range * range;
            }
            return false;
        }
    };

    IdleBotPlayerbotBridge* CreateBridge(std::string const& /*controlMode*/)
    {
        // Only the internal bridge exists today; a chat-command variant is not
        // viable master-less (see file header). controlMode is reserved.
        return new IdleBotInternalBridge();
    }
}
