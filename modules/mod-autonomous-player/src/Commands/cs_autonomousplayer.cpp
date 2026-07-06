/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Admin/console commands for mod-autonomous-player.
//
// `provision` and `login` are the test-setup surface described in
// ARCHITECTURE.md ADR-005/ADR-008: they are only ever invoked explicitly
// by a human GM/console operator, never by the bot runtime loop itself.
// `status` is read-only and safe to run any time.

#include "Chat.h"
#include "Bag.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "Combat/BotCombat.h"
#include "CommandScript.h"
#include "Common.h"
#include "Creature.h"
#include "Economy/BotEconomy.h"
#include "EncounterModel/BotEncounterModel.h"
#include "GossipDef.h"
#include "Gossip/BotGossip.h"
#include "GridNotifiers.h"
#include "Growth/BotGrowth.h"
#include "GuideRuntime/BotGuideRuntime.h"
#include "Inventory/BotLoot.h"
#include "Lifecycle/BotLifecycleMgr.h"
#include "Lifecycle/BotLogin.h"
#include "Transport/BotTransport.h"
#include "Transport.h"
#include "Lifecycle/BotSessionMgr.h"
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Perception/PerceptionBuilder.h"
#include "Pets/BotPets.h"
#include "Player.h"
#include "QuestEngine/BotQuestEngine.h"
#include "Recovery/BotRecovery.h"
#include "Setup/BotProvisioning.h"
#include "Setup/PendingCharacterCreations.h"
#include "SpellMgr.h"

#include <cstdlib>
#include <list>
#include <map>
#include <optional>
#include <sstream>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    class AutonomousPlayerCommandScript : public CommandScript
    {
    public:
        AutonomousPlayerCommandScript() : CommandScript("AutonomousPlayerCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable autonomousPlayerCommandTable =
            {
                { "provision", HandleProvisionCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "login",     HandleLoginCommand,     SEC_ADMINISTRATOR, Console::Yes },
                { "logout",    HandleLogoutCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "status",    HandleStatusCommand,    SEC_GAMEMASTER,    Console::Yes },
                { "transportinfo", HandleTransportInfoCommand, SEC_GAMEMASTER, Console::Yes },
                { "moveto",    HandleMoveToCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "acceptquest", HandleAcceptQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "queststatus", HandleQuestStatusCommand, SEC_GAMEMASTER,    Console::Yes },
                { "completequest", HandleCompleteQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "giveitem",  HandleGiveItemCommand,   SEC_ADMINISTRATOR, Console::Yes },
                { "baginfo",   HandleBagInfoCommand,    SEC_GAMEMASTER,    Console::Yes },
                { "turnin",    HandleTurnInCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "resetquest", HandleResetQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "forcequest", HandleForceQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "attack",    HandleAttackCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "creaturestatus", HandleCreatureStatusCommand, SEC_GAMEMASTER, Console::Yes },
                { "targetsafety", HandleTargetSafetyCommand, SEC_GAMEMASTER, Console::Yes },
                { "loot",      HandleLootCommand,      SEC_ADMINISTRATOR, Console::Yes },
                { "releasespirit", HandleReleaseSpiritCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "reclaimcorpse", HandleReclaimCorpseCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "attackguid", HandleAttackGuidCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "multipull", HandleMultiPullCommand,  SEC_ADMINISTRATOR, Console::Yes },
                { "buy",       HandleBuyCommand,        SEC_ADMINISTRATOR, Console::Yes },
                { "buyupgrades", HandleBuyUpgradesCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "repair",    HandleRepairCommand,     SEC_ADMINISTRATOR, Console::Yes },
                { "gossiphello", HandleGossipHelloCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "gossiptrain", HandleGossipTrainCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "learnspell", HandleLearnSpellCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "equipupgrades", HandleEquipUpgradesCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "castspell", HandleCastSpellCommand,  SEC_ADMINISTRATOR, Console::Yes },
                { "spellbook", HandleSpellbookCommand,  SEC_GAMEMASTER,    Console::Yes },
                { "guidestart", HandleGuideStartCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartmoveto", HandleGuideStartMoveToCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartcombat", HandleGuideStartCombatCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartcombatability", HandleGuideStartCombatAbilityCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartgrind", HandleGuideStartGrindCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "spirithealres", HandleSpiritHealResCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "repopgraveyard", HandleRepopGraveyardCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "hearth", HandleHearthCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartquest", HandleGuideStartQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartquestgrind", HandleGuideStartQuestGrindCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartselljunk", HandleGuideStartSellJunkCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartgameobject", HandleGuideStartGameObjectCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartuseitemunit", HandleGuideStartUseItemUnitCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartuseitemlocation", HandleGuideStartUseItemLocationCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartareatrigger", HandleGuideStartAreaTriggerCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestarttransport", HandleGuideStartTransportCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestatus", HandleGuideStatusCommand, SEC_GAMEMASTER, Console::Yes },
                { "threats", HandleThreatsCommand, SEC_GAMEMASTER, Console::Yes },
                { "encountersnapshot", HandleEncounterSnapshotCommand, SEC_GAMEMASTER, Console::Yes },
                { "tamebeast", HandleTameBeastCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "petstatus", HandlePetStatusCommand, SEC_GAMEMASTER, Console::Yes },
                { "petreactstate", HandlePetReactStateCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "revivepet", HandleRevivePetCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "dismisspet", HandleDismissPetCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "abandonpet", HandleAbandonPetCommand, SEC_ADMINISTRATOR, Console::Yes },
            };
            static ChatCommandTable commandTable =
            {
                { "autonomousplayer", autonomousPlayerCommandTable },
            };
            return commandTable;
        }

        // .autonomousplayer provision <account> <password> <charname> <race> <class> <gender>
        static bool HandleProvisionCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->PSendSysMessage(
                    "Usage: .autonomousplayer provision <account> <password> <charname> <race> <class> <gender> "
                    "(account must start with '{}')",
                    AutonomousPlayer::Setup::AccountPrefix);
                return false;
            }

            std::istringstream stream(args);
            std::string account, password, charName;
            uint32 race = 0, characterClass = 0, gender = 0;

            if (!(stream >> account >> password >> charName >> race >> characterClass >> gender))
            {
                handler->PSendSysMessage(
                    "Usage: .autonomousplayer provision <account> <password> <charname> <race> <class> <gender> "
                    "(account must start with '{}')",
                    AutonomousPlayer::Setup::AccountPrefix);
                return false;
            }

            // ADR-032: check the name BEFORE submitting -- a name
            // `HandleCharCreateOpcode` would reject (e.g. digits, which
            // are not allowed in a real character name) returns before
            // ever reaching the async DB chain, and its rejection packet
            // is a silent no-op for a null-socket bot session. Without
            // this, that failure mode is indistinguishable from a hung
            // creation until `PendingCharacterCreations` times out
            // several minutes later with no useful error (found live,
            // see KNOWN_FAILURES.md #9).
            std::string nameError = AutonomousPlayer::Setup::ValidateCharacterName(charName);
            if (!nameError.empty())
            {
                handler->PSendSysMessage(
                    "Refusing to submit character creation for '{}': {}.", charName, nameError);
                return true;
            }

            uint32 accountId = AutonomousPlayer::Setup::EnsureBotAccount(account, password);
            if (!accountId)
            {
                handler->PSendSysMessage("Failed to create/find account '{}'.", account);
                return true;
            }

            WorldSession* session = AutonomousPlayer::Setup::CreateBotSession(accountId, account);

            // Must be tracked BEFORE submitting the (async) creation
            // request, or the DB query chain never gets pumped -- see
            // ARCHITECTURE.md ADR-008.
            sBotSessionMgr->TrackSession(session);
            AutonomousPlayer::Setup::SubmitCharacterCreate(
                session, charName, uint8(race), uint8(characterClass), uint8(gender));
            AutonomousPlayer::Setup::PendingCharacterCreations::Watch(session, charName);

            handler->PSendSysMessage(
                "Submitted character creation for '{}' on account '{}' (id {}). "
                "Check `.autonomousplayer status` or the server log in a few seconds.",
                charName, account, accountId);
            return true;
        }

        // .autonomousplayer login <account> <charname>
        static bool HandleLoginCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer login <account> <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string account, charName;

            if (!(stream >> account >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer login <account> <charname>");
                return false;
            }

            bool submitted = AutonomousPlayer::Lifecycle::TryLoginBot(account, charName);
            handler->PSendSysMessage(
                submitted
                    ? "Login request submitted for '{}'."
                    : "Login request NOT submitted for '{}' (see log for reason).",
                charName);
            return true;
        }

        // .autonomousplayer transportinfo <charname> <transportEntry>
        //
        // Read-only probe for the transport-boarding work: reports whether the
        // MO_TRANSPORT of <transportEntry> is on the bot's map right now, its
        // live position, and whether the bot is aboard -- exercises the
        // TransportBehaviors primitives without moving/boarding anything.
        static bool HandleTransportInfoCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 entry = 0;
            if (!(stream >> charName >> entry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer transportinfo <charname> <transportEntry>");
                return false;
            }
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }
            Transport* transport = AutonomousPlayer::TransportBehaviors::FindTransport(player, entry);
            if (!transport)
            {
                handler->PSendSysMessage(
                    "Transport {} is NOT on '{}'s map ({}) right now (mid-route or wrong map).",
                    entry, charName, player->GetMapId());
                return true;
            }
            handler->PSendSysMessage(
                "Transport {} '{}' on map {} at ({:.1f}, {:.1f}, {:.1f}); bot dist {:.1f}yd; bot onTransport={}",
                entry, transport->GetName(), transport->GetMapId(),
                transport->GetPositionX(), transport->GetPositionY(), transport->GetPositionZ(),
                player->GetDistance(transport),
                AutonomousPlayer::TransportBehaviors::IsOnTransport(player));
            return true;
        }

        // .autonomousplayer logout <charname>
        //
        // Closes KNOWN_FAILURES.md #23: `.kick` is a silent no-op for a
        // socketless bot session (kick processing lives in the
        // session-update path BotSessionMgr deliberately drives with a
        // MapSessionFilter, which never processes it), so before this
        // command existed the only way to recycle a bot session was a
        // full worldserver restart. Calls WorldSession::LogoutPlayer
        // directly -- the same real teardown a genuine logout performs
        // (character saved, removed from world, OnPlayerLogout fires) --
        // which routes around the filtered path entirely. Session
        // deletion then happens exactly like an organic logout: the
        // module's own OnPlayerLogout hook queues it via
        // QueueForRemoval, deferred to the next BotSessionMgr::Update,
        // safely off this command's call stack.
        static bool HandleLogoutCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer logout <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            if (!(stream >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer logout <charname>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            Player* bot = ObjectAccessor::FindPlayer(guid);
            WorldSession* session = bot ? bot->GetSession() : nullptr;
            if (!session || !session->IsBot()
                || !AutonomousPlayer::Setup::IsAutonomousPlayerAccount(session->GetAccountId()))
            {
                handler->PSendSysMessage(
                    "'{}' is registered but not resolvable to an online bot session this module owns.",
                    charName);
                return true;
            }

            session->LogoutPlayer(true); // save=true; fires OnPlayerLogout
            handler->PSendSysMessage(
                "'{}' logged out and saved. The session is queued for deletion; "
                "`.autonomousplayer login` can bring the character back immediately.",
                charName);
            return true;
        }

        // .autonomousplayer moveto <charname> <x> <y> <z>
        //
        // Debug-only trigger for the Navigation component (Gate 2 first
        // slice): not part of any Planner/Executor loop yet, just a way to
        // prove MoveTo/MotionMaster movement works end-to-end on a live
        // online bot before building quest-walk logic on top of it.
        static bool HandleMoveToCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer moveto <charname> <x> <y> <z>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            float x = 0.f, y = 0.f, z = 0.f;

            if (!(stream >> charName >> x >> y >> z))
            {
                handler->SendSysMessage("Usage: .autonomousplayer moveto <charname> <x> <y> <z>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty())
            {
                handler->PSendSysMessage("No such character '{}'.", charName);
                return true;
            }

            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(player, x, y, z);
            handler->PSendSysMessage("Moving '{}' toward ({:.1f}, {:.1f}, {:.1f}).", charName, x, y, z);
            return true;
        }

        // .autonomousplayer acceptquest <charname> <questId> <questGiverEntry>
        //
        // Debug-only trigger for the QuestEngine component (Gate 2 next
        // slice): finds the nearest creature with `questGiverEntry` near
        // the bot and submits a real quest-accept request for `questId`
        // via the same public opcode handler a client uses.
        static bool HandleAcceptQuestCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer acceptquest <charname> <questId> <questGiverEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 questId = 0, questGiverEntry = 0;

            if (!(stream >> charName >> questId >> questGiverEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer acceptquest <charname> <questId> <questGiverEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* questGiver = player->FindNearestCreature(questGiverEntry, 30.0f);
            if (!questGiver)
            {
                handler->PSendSysMessage("No creature with entry {} within 30 yards of '{}'.", questGiverEntry, charName);
                return true;
            }

            AutonomousPlayer::QuestEngine::RequestAcceptQuest(player, questId, questGiver->GetGUID());
            handler->PSendSysMessage(
                "Submitted quest-accept for quest {} from '{}' ({}) to '{}'. Check quest status.",
                questId, questGiver->GetName(), questGiver->GetGUID().ToString(), charName);
            return true;
        }

        // .autonomousplayer turnin <charname> <questId> <questGiverEntry> <rewardChoiceIndex>
        //
        // Debug-only trigger for QuestEngine turn-in (Gate 2 slice 3):
        // finds the nearest creature with `questGiverEntry` and submits a
        // real turn-in/reward-choice request for `questId`. The quest
        // must already be QUEST_STATUS_COMPLETE (see acceptquest).
        static bool HandleTurnInCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer turnin <charname> <questId> <questGiverEntry> <rewardChoiceIndex>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 questId = 0, questGiverEntry = 0, rewardChoiceIndex = 0;

            if (!(stream >> charName >> questId >> questGiverEntry >> rewardChoiceIndex))
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer turnin <charname> <questId> <questGiverEntry> <rewardChoiceIndex>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* questGiver = player->FindNearestCreature(questGiverEntry, 30.0f);
            if (!questGiver)
            {
                handler->PSendSysMessage("No creature with entry {} within 30 yards of '{}'.", questGiverEntry, charName);
                return true;
            }

            AutonomousPlayer::QuestEngine::RequestChooseReward(
                player, questId, questGiver->GetGUID(), rewardChoiceIndex);
            handler->PSendSysMessage(
                "Submitted turn-in for quest {} to '{}' ({}) from '{}'. Check IsQuestRewarded / XP.",
                questId, questGiver->GetName(), questGiver->GetGUID().ToString(), charName);
            return true;
        }

        // .autonomousplayer giveitem <charname> <itemId> [count]
        //
        // Provision a bot with an item (bags, ammo, reagents) it can't
        // easily acquire autonomously yet. EquipBagUpgrades then equips
        // looted/gifted bags into free bag slots on the next growth pass.
        static bool HandleGiveItemCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 itemId = 0, count = 1;
            if (!(stream >> charName >> itemId))
            {
                handler->SendSysMessage("Usage: .autonomousplayer giveitem <charname> <itemId> [count]");
                return false;
            }
            if (!(stream >> count) || count == 0)
            {
                count = 1;
            }
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }
            bool ok = player->AddItem(itemId, count);
            handler->PSendSysMessage(
                "giveitem {} x{} to '{}': {}", itemId, count, charName, ok);
            return true;
        }

        // .autonomousplayer baginfo <charname>
        //
        // Diagnostic for the bag-pressure emergency path: lists the bag
        // contents grouped by item, so a bot that stays full after
        // vendoring reports exactly what is clogging it (entry, name,
        // quality, class/subclass, count, sell price).
        static bool HandleBagInfoCommand(ChatHandler* handler, char const* args)
        {
            std::string charName(args ? args : "");
            while (!charName.empty() && charName.back() == ' ')
            {
                charName.pop_back();
            }
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            std::map<uint32, uint32> counts;
            auto tally = [&counts](Item* item)
            {
                if (item)
                {
                    counts[item->GetEntry()] += item->GetCount();
                }
            };
            for (uint8 s = INVENTORY_SLOT_ITEM_START; s < INVENTORY_SLOT_ITEM_END; ++s)
            {
                tally(player->GetItemByPos(INVENTORY_SLOT_BAG_0, s));
            }
            for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
            {
                if (Bag* bag = player->GetBagByPos(b))
                {
                    for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                    {
                        tally(bag->GetItemByPos(i));
                    }
                }
            }

            handler->PSendSysMessage("baginfo '{}': {} distinct item(s)", charName, counts.size());
            for (auto const& [entry, count] : counts)
            {
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                if (!proto)
                {
                    continue;
                }
                handler->PSendSysMessage(
                    "  {}x [{}] '{}' q{} class {}/{} sell {}",
                    count, entry, proto->Name1, proto->Quality,
                    proto->Class, proto->SubClass, proto->SellPrice);
            }
            return true;
        }

        // .autonomousplayer resetquest <charname> <questId>
        //
        // Intervention after a fix: fully re-do a quest. Removes it from
        // the log AND the rewarded set and clears its status, so the bot
        // re-accepts and re-completes it from scratch. Use once a
        // route/nav/target bug is fixed so the bot re-attempts cleanly
        // instead of treating a half-broken or wrongly-skipped state as
        // done. (Reposition the bot to its hub with `teleport name` so it
        // re-picks the quest up from a sane spot.)
        static bool HandleResetQuestCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0;
            if (!(stream >> charName >> questId))
            {
                handler->SendSysMessage("Usage: .autonomousplayer resetquest <charname> <questId>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                handler->PSendSysMessage("No such quest {}.", questId);
                return true;
            }

            uint16 slot = player->FindQuestSlot(questId);
            if (slot < MAX_QUEST_LOG_SIZE)
            {
                player->SetQuestSlot(slot, 0);
                player->RemoveActiveQuest(questId, false);
            }
            player->RemoveRewardedQuest(questId);
            player->SetQuestStatus(questId, QUEST_STATUS_NONE);
            handler->PSendSysMessage(
                "Reset quest {} for '{}': status NONE, rewarded cleared -- re-acceptable now.",
                questId, charName);
            return true;
        }

        // .autonomousplayer forcequest <charname> <questId>
        //
        // Escape hatch for a quest the bot genuinely CANNOT perform
        // (custom behavior: escort, use-object, area trigger, event
        // script) that gates a chain we still want. Diagnose FIRST -- a
        // hard-but-doable quest gets fixed and re-run, not forced. This
        // adds the quest, completes its objectives, and rewards it so the
        // chain's prerequisite is satisfied.
        static bool HandleForceQuestCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0;
            if (!(stream >> charName >> questId))
            {
                handler->SendSysMessage("Usage: .autonomousplayer forcequest <charname> <questId>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
            {
                handler->PSendSysMessage("No such quest {}.", questId);
                return true;
            }

            if (!player->IsQuestRewarded(questId))
            {
                if (player->FindQuestSlot(questId) == MAX_QUEST_LOG_SIZE)
                {
                    player->AddQuest(quest, player);
                }
                player->CompleteQuest(questId);
                player->RewardQuest(quest, 0, player, false);
            }
            handler->PSendSysMessage(
                "Forced quest {} for '{}': rewarded={} (prerequisite satisfied).",
                questId, charName, player->IsQuestRewarded(questId));
            return true;
        }

        // .autonomousplayer attack <charname> <creatureEntry>
        //
        // Debug-only trigger for the Combat component (Gate 2 slice 4):
        // finds the nearest creature with `creatureEntry`, walks the bot
        // to melee range of it (Navigation::MoveTo -- real pathing, not a
        // teleport), then submits a real attack-start request via the
        // same opcode handler a client uses. Does not select/validate the
        // target for legality beyond "closest of this entry" -- real
        // target selection is later Combat-component scope.
        static bool HandleAttackCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer attack <charname> <creatureEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;

            if (!(stream >> charName >> creatureEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer attack <charname> <creatureEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* target = player->FindNearestCreature(creatureEntry, 100.0f);
            if (!target)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", creatureEntry, charName);
                return true;
            }

            // Close to melee range first (real movement, not a teleport)
            // so the attack isn't started from an unrealistic distance.
            AutonomousPlayer::Navigation::MoveTo(
                player, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());

            AutonomousPlayer::Combat::RequestAttack(player, target->GetGUID());
            handler->PSendSysMessage(
                "Moving to and attacking '{}' ({}, entry {}, {}/{} hp) with '{}'.",
                target->GetName(), target->GetGUID().ToString(), creatureEntry,
                target->GetHealth(), target->GetMaxHealth(), charName);
            return true;
        }

        // .autonomousplayer attackguid <charname> <creatureEntry> <lowGuid>
        //
        // Pure test/debug convenience (not part of the module's real
        // Combat interface -- see Combat::RequestAttack for that): lets a
        // test operator target one *specific* creature instead of always
        // "nearest of this entry" (used to deliberately multi-pull
        // several distinct creatures for Recovery-slice death testing).
        static bool HandleAttackGuidCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer attackguid <charname> <creatureEntry> <lowGuid>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;
            uint32 lowGuid = 0;

            if (!(stream >> charName >> creatureEntry >> lowGuid))
            {
                handler->SendSysMessage("Usage: .autonomousplayer attackguid <charname> <creatureEntry> <lowGuid>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            ObjectGuid targetGuid = ObjectGuid::Create<HighGuid::Unit>(creatureEntry, lowGuid);
            Creature* target = player->GetMap()->GetCreature(targetGuid);
            if (!target)
            {
                handler->PSendSysMessage("No creature {} (entry {}) found on '{}'s map.", lowGuid, creatureEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(
                player, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            AutonomousPlayer::Combat::RequestAttack(player, target->GetGUID());
            handler->PSendSysMessage(
                "Moving to and attacking '{}' ({}, {}/{} hp) with '{}'.",
                target->GetName(), target->GetGUID().ToString(),
                target->GetHealth(), target->GetMaxHealth(), charName);
            return true;
        }

        // .autonomousplayer multipull <charname> <creatureEntry> <range> <count>
        //
        // Pure test/debug convenience (not part of the module's real
        // Combat interface): deliberately engages up to `count` distinct
        // creatures of `creatureEntry` within `range` yards, to exercise
        // unsafe-pack-density scenarios for Recovery-slice death testing.
        // Uses WorldObject::GetCreatureListWithEntryInGrid for real
        // Creature* pointers, since guessing live in-game GUIDs from the
        // static creature-spawn table's `guid` column doesn't reliably
        // match this fork's runtime GUID assignment.
        static bool HandleMultiPullCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer multipull <charname> <creatureEntry> <range> <count>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;
            float range = 0.f;
            uint32 count = 0;

            if (!(stream >> charName >> creatureEntry >> range >> count))
            {
                handler->SendSysMessage("Usage: .autonomousplayer multipull <charname> <creatureEntry> <range> <count>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            std::list<Creature*> creatures;
            player->GetCreatureListWithEntryInGrid(creatures, creatureEntry, range);

            uint32 engaged = 0;
            for (Creature* target : creatures)
            {
                if (engaged >= count || !target->IsAlive())
                {
                    continue;
                }

                AutonomousPlayer::Combat::RequestAttack(player, target->GetGUID());
                handler->PSendSysMessage(
                    "  engaging '{}' ({}, {}/{} hp)",
                    target->GetName(), target->GetGUID().ToString(),
                    target->GetHealth(), target->GetMaxHealth());
                ++engaged;
            }

            handler->PSendSysMessage("Engaged {} of {} requested (entry {} within {} yards).",
                engaged, count, creatureEntry, range);
            return true;
        }

        // .autonomousplayer creaturestatus <charname> <creatureEntry>
        static bool HandleCreatureStatusCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer creaturestatus <charname> <creatureEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;

            if (!(stream >> charName >> creatureEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer creaturestatus <charname> <creatureEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            // FindNearestCreature's `alive` param is an exact match
            // (Creature::IsAlive() == alive), not "include both" when
            // false -- the original `false` here actually meant "only
            // dead," a real footgun found via `targetsafety`
            // (KNOWN_FAILURES.md #8) and fixed there first; applying the
            // same alive-then-dead fallback here closes the gap in this
            // command too.
            Creature* target = player->FindNearestCreature(creatureEntry, 100.0f, true);
            if (!target)
            {
                target = player->FindNearestCreature(creatureEntry, 100.0f, false);
            }
            if (!target)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}' (dead or alive).",
                    creatureEntry, charName);
                return true;
            }

            handler->PSendSysMessage(
                "'{}' ({}) hp {}/{} alive={} pos ({:.1f}, {:.1f}, {:.1f})",
                target->GetName(), target->GetGUID().ToString(),
                target->GetHealth(), target->GetMaxHealth(), target->IsAlive(),
                target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            return true;
        }

        // .autonomousplayer targetsafety <charname> <creatureEntry>
        //
        // Diagnostic for ADR-031's target-selection safety checks
        // (GuideRuntime::IsSafeToEngage) -- reports each individual
        // real-engine-state check for the nearest matching creature
        // (dead or alive) instead of just the pass/fail
        // KillNearest itself would apply, so a specific failure mode can
        // be confirmed live rather than inferred from "nothing got
        // attacked."
        static bool HandleTargetSafetyCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer targetsafety <charname> <creatureEntry> [range=100]");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;
            float range = 100.0f;

            if (!(stream >> charName >> creatureEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer targetsafety <charname> <creatureEntry> [range=100]");
                return false;
            }
            float parsedRange = 0.0f;
            if (stream >> parsedRange) // optional; C++11 sets parsedRange=0 and fails on absent/bad input, so only apply on success
            {
                range = parsedRange;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            // FindNearestCreature's `alive` param is an exact match
            // (Creature::IsAlive() == alive), not "include both" when
            // false -- `creaturestatus`'s existing `false` argument
            // actually means "only dead," a pre-existing footgun this
            // command deliberately avoids by searching alive, then dead,
            // separately so a real live target is what gets diagnosed.
            Creature* target = player->FindNearestCreature(creatureEntry, range, true);
            if (!target)
            {
                target = player->FindNearestCreature(creatureEntry, range, false);
            }
            if (!target)
            {
                handler->PSendSysMessage("No creature with entry {} within {} yards of '{}' (dead or alive).",
                    creatureEntry, range, charName);
                return true;
            }

            bool alive = target->IsAlive();
            bool evading = target->IsInEvadeMode();
            // IsValidAttackTarget, not IsHostileTo -- most low-level
            // questing wildlife (Mottled Boar confirmed live) is
            // faction-neutral, not Hostile, yet a legitimate kill target;
            // see IsSafeToEngage's comment in BotGuideRuntime.cpp.
            bool attackable = player->IsValidAttackTarget(target);
            bool hasLootRecipient = target->hasLootRecipient();
            bool tappedByBot = target->isTappedBy(player);
            bool otherPlayerAttacking = false;
            for (Unit* attacker : target->getAttackers())
            {
                if (attacker && attacker->IsPlayer() && attacker != player)
                {
                    otherPlayerAttacking = true;
                    break;
                }
            }
            // Must match `IsSafeToEngage`'s real check exactly
            // (`ModelIgnoreFlags::M2`, KNOWN_FAILURES.md #21) -- this
            // command exists to diagnose that policy, and a stricter
            // diagnostic here would report los=false for targets the
            // policy genuinely accepts.
            bool los = player->IsWithinLOSInMap(target, VMAP::ModelIgnoreFlags::M2);

            bool safe = alive && !evading && attackable && (!hasLootRecipient || tappedByBot) &&
                        !otherPlayerAttacking && los;

            handler->PSendSysMessage(
                "'{}' ({}) alive={} evading={} attackable={} hasLootRecipient={} tappedByBot={} "
                "otherPlayerAttacking={} los={} -> safe={}",
                target->GetName(), target->GetGUID().ToString(), alive, evading, attackable,
                hasLootRecipient, tappedByBot, otherPlayerAttacking, los, safe);
            return true;
        }

        // .autonomousplayer loot <charname> <creatureEntry>
        //
        // Debug-only trigger for the Inventory component's loot slice
        // (Gate 2 slice 5): finds the nearest DEAD creature with
        // `creatureEntry`, walks the bot to loot range, then loots it via
        // real opcode-handler reuse (see Inventory::LootCorpse).
        static bool HandleLootCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer loot <charname> <creatureEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;

            if (!(stream >> charName >> creatureEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer loot <charname> <creatureEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* corpse = player->FindNearestCreature(creatureEntry, 100.0f, false);
            if (!corpse || corpse->IsAlive())
            {
                handler->PSendSysMessage("No dead creature with entry {} within 100 yards of '{}'.", creatureEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(
                player, corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());

            uint32 moneyBefore = player->GetMoney();
            bool ok = AutonomousPlayer::Inventory::LootCorpse(player, corpse);
            handler->PSendSysMessage(
                "Loot request for '{}' ({}) by '{}': submitted={}, money before={}, money after={}",
                corpse->GetName(), corpse->GetGUID().ToString(), charName, ok, moneyBefore, player->GetMoney());
            return true;
        }

        // .autonomousplayer releasespirit <charname>
        //
        // Debug-only trigger for the Recovery component's first slice
        // (Gate 2 slice 6): submits a real release-spirit request. Only
        // works while the bot is dead.
        static bool HandleReleaseSpiritCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer releasespirit <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool submitted = AutonomousPlayer::Recovery::RequestReleaseSpirit(player);
            handler->PSendSysMessage(
                "Release-spirit request for '{}': submitted={}, alive={}, ghost={}",
                charName, submitted, player->IsAlive(), player->HasPlayerFlag(PLAYER_FLAGS_GHOST));
            return true;
        }

        // .autonomousplayer spirithealres <charname>
        //
        // Spirit-healer resurrection (real opcode; sickness +
        // durability apply) -- the honest last resort for a corpse no
        // ghost can walk to.
        static bool HandleSpiritHealResCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer spirithealres <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool submitted = AutonomousPlayer::Recovery::RequestSpiritHealerResurrect(player);
            handler->PSendSysMessage(
                "Spirit-healer resurrect for '{}': submitted={}, alive={}",
                charName, submitted, player->IsAlive());
            return true;
        }

        // .autonomousplayer repopgraveyard <charname>
        //
        // Port a stranded ghost to its zone's nearest graveyard (the
        // same Player::RepopAtGraveyard call spirit release runs), so
        // the spirit healer there can resurrect it.
        static bool HandleRepopGraveyardCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer repopgraveyard <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool submitted = AutonomousPlayer::Recovery::ReturnGhostToGraveyard(player);
            handler->PSendSysMessage(
                "Graveyard repop for '{}': submitted={} (still a ghost; spirithealres next).",
                charName, submitted);
            return true;
        }

        // .autonomousplayer hearth <charname>
        static bool HandleHearthCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer hearth <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool submitted = AutonomousPlayer::Recovery::RequestUseHearthstone(player);
            handler->PSendSysMessage(
                "Hearthstone for '{}': submitted={} (10s cast; check map/pos after).",
                charName, submitted);
            return true;
        }

        // .autonomousplayer reclaimcorpse <charname>
        //
        // Debug-only trigger for corpse reclaim/resurrect. Only works if
        // the bot is a ghost, its corpse still exists, ~30s have passed
        // since release, and it's within 39 yards of the corpse (walk it
        // there first via .autonomousplayer moveto).
        static bool HandleReclaimCorpseCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer reclaimcorpse <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool submitted = AutonomousPlayer::Recovery::RequestReclaimCorpse(player);
            handler->PSendSysMessage(
                "Reclaim-corpse request for '{}': submitted={}, alive={}",
                charName, submitted, player->IsAlive());
            return true;
        }

        // .autonomousplayer buyupgrades <charname> <vendorEntry>
        //
        // Buys every per-slot weapon/armor upgrade the bot can use and
        // afford from the vendor's stock, then equips them -- the
        // gear-floor fix (see Economy::BuyGearUpgrades).
        static bool HandleBuyUpgradesCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 vendorEntry = 0;
            if (!(stream >> charName >> vendorEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer buyupgrades <charname> <vendorEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* vendor = player->FindNearestCreature(vendorEntry, 100.0f);
            if (!vendor)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", vendorEntry, charName);
                return true;
            }

            uint32 moneyBefore = player->GetMoney();
            uint32 bought = AutonomousPlayer::Economy::BuyGearUpgrades(player, vendor);
            uint32 consumables = AutonomousPlayer::Economy::BuyConsumables(player, vendor, 10);
            uint32 equipped = AutonomousPlayer::Growth::EquipBagUpgrades(player);
            handler->PSendSysMessage(
                "Gear upgrades from '{}' for '{}': bought={}, consumables={}, equipped={}, money {} -> {}.",
                vendor->GetName(), charName, bought, consumables, equipped, moneyBefore, player->GetMoney());
            return true;
        }

        // .autonomousplayer buy <charname> <vendorEntry> <itemId> <count>
        //
        // Debug-only trigger for the Economy component's first slice
        // (Gate 2 slice 7): walks the bot to the nearest creature with
        // `vendorEntry` and submits a real buy request for `itemId` via
        // Economy::BuyItem.
        static bool HandleBuyCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer buy <charname> <vendorEntry> <itemId> <count>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 vendorEntry = 0, itemId = 0, count = 0;

            if (!(stream >> charName >> vendorEntry >> itemId >> count))
            {
                handler->SendSysMessage("Usage: .autonomousplayer buy <charname> <vendorEntry> <itemId> <count>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* vendor = player->FindNearestCreature(vendorEntry, 100.0f);
            if (!vendor)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", vendorEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(player, vendor->GetPositionX(), vendor->GetPositionY(), vendor->GetPositionZ());

            uint32 moneyBefore = player->GetMoney();
            bool ok = AutonomousPlayer::Economy::BuyItem(player, vendor, itemId, count);
            handler->PSendSysMessage(
                "Buy request for item {} x{} from '{}' by '{}': submitted={}, money before={}, money after={}",
                itemId, count, vendor->GetName(), charName, ok, moneyBefore, player->GetMoney());
            return true;
        }

        // .autonomousplayer repair <charname> <vendorEntry>
        static bool HandleRepairCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer repair <charname> <vendorEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 vendorEntry = 0;

            if (!(stream >> charName >> vendorEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer repair <charname> <vendorEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* vendor = player->FindNearestCreature(vendorEntry, 100.0f);
            if (!vendor)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", vendorEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(player, vendor->GetPositionX(), vendor->GetPositionY(), vendor->GetPositionZ());

            uint32 moneyBefore = player->GetMoney();
            bool ok = AutonomousPlayer::Economy::RepairAll(player, vendor);
            handler->PSendSysMessage(
                "Repair-all request at '{}' by '{}': submitted={}, money before={}, money after={}",
                vendor->GetName(), charName, ok, moneyBefore, player->GetMoney());
            return true;
        }

        // .autonomousplayer gossiphello <charname> <npcEntry>
        //
        // Debug-only trigger for the Gossip component's first slice
        // (Gate 2 slice 8): walks the bot to the nearest creature with
        // `npcEntry`, opens a real gossip dialogue, and lists every menu
        // item's OptionType/message for inspection.
        static bool HandleGossipHelloCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer gossiphello <charname> <npcEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 npcEntry = 0;

            if (!(stream >> charName >> npcEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer gossiphello <charname> <npcEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* npc = player->FindNearestCreature(npcEntry, 100.0f);
            if (!npc)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", npcEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(player, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
            bool ok = AutonomousPlayer::Gossip::RequestGossipHello(player, npc);

            handler->PSendSysMessage("Gossip-hello to '{}' by '{}': submitted={}. Menu items:", npc->GetName(), charName, ok);
            for (auto const& [id, item] : player->PlayerTalkClass->GetGossipMenu().GetMenuItems())
            {
                handler->PSendSysMessage("  [{}] optionType={} \"{}\"", id, item.OptionType, item.Message);
            }
            return true;
        }

        // .autonomousplayer gossiptrain <charname> <npcEntry>
        //
        // Debug-only combined trigger: gossip-hello then finds and
        // selects the GOSSIP_OPTION_TRAINER menu item, if any.
        static bool HandleGossipTrainCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer gossiptrain <charname> <npcEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 npcEntry = 0;

            if (!(stream >> charName >> npcEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer gossiptrain <charname> <npcEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* npc = player->FindNearestCreature(npcEntry, 100.0f);
            if (!npc)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", npcEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(player, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
            AutonomousPlayer::Gossip::RequestGossipHello(player, npc);

            std::optional<uint32> trainerOption =
                AutonomousPlayer::Gossip::FindGossipOptionIndex(player, GOSSIP_OPTION_TRAINER);
            if (!trainerOption)
            {
                handler->PSendSysMessage("'{}' has no GOSSIP_OPTION_TRAINER menu item for '{}'.", npc->GetName(), charName);
                return true;
            }

            bool ok = AutonomousPlayer::Gossip::RequestGossipSelectOption(player, npc, *trainerOption);
            handler->PSendSysMessage(
                "Selected trainer option [{}] on '{}' for '{}': submitted={}. Check IsInWorld/trainer session state.",
                *trainerOption, npc->GetName(), charName, ok);
            return true;
        }

        // .autonomousplayer learnspell <charname> <trainerEntry>
        //
        // Debug-only trigger for the Growth component's first slice
        // (Gate 2 slice 9): walks the bot to the nearest creature with
        // `trainerEntry`, opens the trainer list, finds the first spell
        // the bot can actually learn right now, and requests to learn it.
        static bool HandleLearnSpellCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer learnspell <charname> <trainerEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 trainerEntry = 0;

            if (!(stream >> charName >> trainerEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer learnspell <charname> <trainerEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* trainer = player->FindNearestCreature(trainerEntry, 100.0f);
            if (!trainer)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", trainerEntry, charName);
                return true;
            }

            AutonomousPlayer::Navigation::MoveTo(player, trainer->GetPositionX(), trainer->GetPositionY(), trainer->GetPositionZ());
            AutonomousPlayer::Growth::RequestTrainerList(player, trainer);

            std::optional<uint32> spellId = AutonomousPlayer::Growth::FindLearnableTrainerSpell(player, trainer);
            if (!spellId)
            {
                handler->PSendSysMessage("'{}' has no spell '{}' can learn right now.", trainer->GetName(), charName);
                return true;
            }

            bool hadSpellBefore = player->HasSpell(*spellId);
            uint32 moneyBefore = player->GetMoney();
            bool ok = AutonomousPlayer::Growth::RequestLearnSpell(player, trainer, *spellId);
            handler->PSendSysMessage(
                "Learn-spell {} from '{}' by '{}': submitted={}, had spell before={}, has spell after={}, money before={}, money after={}",
                *spellId, trainer->GetName(), charName, ok, hadSpellBefore, player->HasSpell(*spellId),
                moneyBefore, player->GetMoney());
            return true;
        }

        // .autonomousplayer equipupgrades <charname>
        //
        // Growth chore (Gate 3): equip every carried weapon/armor item
        // that fills an empty slot or beats the equipped ItemLevel --
        // real CMSG_AUTOEQUIP_ITEM path, engine does the validation,
        // success verified against real Item::IsEquipped state.
        static bool HandleEquipUpgradesCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer equipupgrades <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            if (!(stream >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer equipupgrades <charname>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            uint32 equipped = AutonomousPlayer::Growth::EquipBagUpgrades(player);
            handler->PSendSysMessage("Equipped {} upgrade(s) from '{}' bags.", equipped, charName);
            return true;
        }

        // .autonomousplayer castspell <charname> <spellId> <targetEntry>
        //
        // Debug-only trigger for Combat::RequestCastSpell (Gate 2 slice
        // 10, broader race/class coverage): finds the nearest creature
        // with `targetEntry`, walks the bot to spell range, and requests
        // a real cast via Unit::CastSpell.
        static bool HandleCastSpellCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer castspell <charname> <spellId> <targetEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 spellId = 0, targetEntry = 0;

            if (!(stream >> charName >> spellId >> targetEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer castspell <charname> <spellId> <targetEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* target = player->FindNearestCreature(targetEntry, 100.0f, true);
            if (!target)
            {
                handler->PSendSysMessage("No creature with entry {} within 100 yards of '{}'.", targetEntry, charName);
                return true;
            }

            // ADR-036: only move if actually out of the spell's own real
            // range -- found live investigating Tame Beast (spell 1515):
            // unconditionally re-issuing `Navigation::MoveTo` on every
            // invocation (even when already in range) sets
            // `UNIT_STATE_MOVING` for a tick, and `Unit::CastSpell`
            // rejects with `SPELL_FAILED_MOVING` -- this made every
            // single retry against an already-in-range target fail with
            // "moving", regardless of how many times or how long between
            // calls, since each call re-triggered the same state. Real
            // players naturally stop moving before casting; this debug
            // command previously never let that happen.
            // Second half of the same bug, found live testing Summon Imp
            // (spell 688, KNOWN_FAILURES.md #12 update): a SELF-cast
            // spell's max range is 0, so the old `maxRange <= 0 -> walk
            // anyway` fallback re-issued MoveTo on every invocation and
            // re-created the exact permanent SPELL_FAILED_MOVING loop
            // ADR-036 fixed for the ranged case. A range-0 spell needs
            // no approach at all -- never move for it.
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            float maxRange = spellInfo ? spellInfo->GetMaxRange(true, player) : 0.0f;
            if (maxRange > 0.0f && player->GetDistance(target) > maxRange)
            {
                AutonomousPlayer::Navigation::MoveTo(
                    player, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            }

            uint32 hpBefore = target->GetHealth();
            uint32 rageBefore = player->GetPower(POWER_RAGE);
            SpellCastResult result = AutonomousPlayer::Combat::RequestCastSpell(player, target, spellId);
            handler->PSendSysMessage(
                "Cast spell {} at '{}' by '{}': result={} ({}), target hp before={}, after={}, "
                "caster rage before={}, after={}",
                spellId, target->GetName(), charName, static_cast<uint32>(result),
                result == SPELL_CAST_OK ? "SPELL_CAST_OK" : "rejected", hpBefore, target->GetHealth(),
                rageBefore, player->GetPower(POWER_RAGE));
            return true;
        }

        // .autonomousplayer spellbook <charname>
        //
        // Read-only debug helper: lists the bot's live in-memory spell
        // IDs (Player::GetSpellMap()) -- used to discover what a
        // freshly-created character actually starts with, rather than
        // guessing from memory of game content. Deliberately reads the
        // live object, not `character_spell` in the DB, since that table
        // only reflects the last save and our socketless bot sessions
        // aren't saved by the usual client-driven timers.
        static bool HandleSpellbookCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer spellbook <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            if (!(stream >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer spellbook <charname>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            handler->PSendSysMessage("Spellbook for '{}':", charName);
            for (auto const& entry : player->GetSpellMap())
            {
                handler->PSendSysMessage("  spell {}", entry.first);
            }
            return true;
        }

        // .autonomousplayer guidestart <charname>
        //
        // Gate 3 slice 1 (GuideRuntime, ADR-019): attaches a fixed
        // 3-waypoint patrol to a registered bot and starts automatic
        // execution. No further command is needed -- BotLifecycleMgr's
        // per-second tick advances it on its own. Waypoints are known
        // Valley of Trials landmarks from earlier this session's testing
        // (Kaltunk's spawn area, Frang the trainer, Huklah the vendor).
        static bool HandleGuideStartCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestart <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            if (!(stream >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestart <charname>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::MoveTo, -639.3f, -4230.2f, 38.1f },
                { AutonomousPlayer::GuideRuntime::StepType::MoveTo, -581.7f, -4109.5f, 43.5f },
                { AutonomousPlayer::GuideRuntime::StepType::MoveTo, -618.5f, -4251.7f, 38.7f },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a 3-waypoint guide for '{}'. No further commands needed -- "
                "check `.autonomousplayer guidestatus {}` to watch it advance on its own.",
                charName, charName);
            return true;
        }

        // .autonomousplayer guidestartmoveto <charname> <x> <y> <z>
        //
        // Debug-only single-step MoveTo guide to an arbitrary coordinate
        // -- unlike guidestart's fixed 3-waypoint patrol, this lets a
        // test target a genuinely unreachable point on purpose, to
        // directly exercise the bounded-timeout path (ADR-028) rather
        // than only ever exercising the happy path.
        static bool HandleGuideStartMoveToCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartmoveto <charname> <x> <y> <z>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (!(stream >> charName >> x >> y >> z))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartmoveto <charname> <x> <y> <z>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::MoveTo, x, y, z },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a single MoveTo guide for '{}' toward ({:.1f}, {:.1f}, {:.1f}). No further commands "
                "needed -- check `.autonomousplayer guidestatus {}` to watch it advance (or time out) on its own.",
                charName, x, y, z, charName);
            return true;
        }

        // .autonomousplayer guidestartcombat <charname> <creatureEntry>
        //
        // Gate 3 slice 2: a single-step guide that walks to, kills, and
        // loots the nearest creature of `creatureEntry` -- fully
        // automatically, no further command needed after this one.
        static bool HandleGuideStartCombatCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartcombat <charname> <creatureEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;
            if (!(stream >> charName >> creatureEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartcombat <charname> <creatureEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                // Search radius kept tighter than the 100-yard default
                // used elsewhere in this module -- see KNOWN_FAILURES.md
                // #3: a target found near/past a 100-yard radius has
                // repeatedly caused KillNearest to get permanently stuck,
                // suspected to be a navmesh-reachability gap in
                // FindNearestCreature's straight-line target selection.
                { AutonomousPlayer::GuideRuntime::StepType::KillNearest, 0.0f, 0.0f, 0.0f, creatureEntry, 50.0f },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a walk+kill+loot guide against entry {} for '{}'. No further commands needed -- "
                "check `.autonomousplayer guidestatus {}` to watch it advance on its own.",
                creatureEntry, charName, charName);
            return true;
        }

        // .autonomousplayer guidestartcombatability <charname> <creatureEntry> <spellId>
        //
        // Gate 3 implementation sequence step 3 (ADR-029): same as
        // guidestartcombat, but also tries `spellId` opportunistically
        // once per tick while Engaged, in addition to bare melee -- the
        // smallest possible "class controller" slice.
        static bool HandleGuideStartCombatAbilityCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartcombatability <charname> <creatureEntry> <spellId>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0, spellId = 0;
            if (!(stream >> charName >> creatureEntry >> spellId))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartcombatability <charname> <creatureEntry> <spellId>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::KillNearest, 0.0f, 0.0f, 0.0f, creatureEntry, 50.0f, 0, 0, spellId },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a walk+kill+loot guide against entry {} for '{}', using spell {} opportunistically. "
                "No further commands needed -- check `.autonomousplayer guidestatus {}` to watch it advance on its own.",
                creatureEntry, charName, spellId, charName);
            return true;
        }

        // .autonomousplayer guidestartgrind <charname> <creatureEntry> <count> <spellId>
        //
        // ADR-051: chain <count> kill+loot cycles engine-side in ONE
        // guide -- pure XP grinding between quests previously paid a
        // full external command round-trip per single kill, which on
        // the live 1->12 run cost more wall-clock than the fights.
        static bool HandleGuideStartGrindCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartgrind <charname> <creatureEntry> <count> <spellId>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0, count = 0, spellId = 0, selfHealSpellId = 0;
            if (!(stream >> charName >> creatureEntry >> count) || count == 0)
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartgrind <charname> <creatureEntry> <count> <spellId> [healId]");
                return false;
            }
            stream >> spellId >> selfHealSpellId;

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::KillNearest, 0.0f, 0.0f, 0.0f,
                  creatureEntry, 50.0f, 0, 0, spellId, false, count, selfHealSpellId },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a {}-cycle kill+loot grind against entry {} for '{}'.",
                count, creatureEntry, charName);
            return true;
        }

        // .autonomousplayer guidestartquest <charname> <questId> <questGiverEntry> <killEntry> <turnInEntry> <rewardChoiceIndex>
        //
        // Gate 3 slice 3 (ADR-021): the first FULL automatic quest loop
        // as a single guide -- accept a real quest, kill+loot a real
        // creature, turn the quest in for a real reward -- with zero
        // manual commands after this one.
        static bool HandleGuideStartQuestCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartquest <charname> <questId> <questGiverEntry> "
                    "<killEntry> <turnInEntry> <rewardChoiceIndex>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 questId = 0, questGiverEntry = 0, killEntry = 0, turnInEntry = 0, rewardChoiceIndex = 0;
            if (!(stream >> charName >> questId >> questGiverEntry >> killEntry >> turnInEntry >> rewardChoiceIndex))
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartquest <charname> <questId> <questGiverEntry> "
                    "<killEntry> <turnInEntry> <rewardChoiceIndex>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::AcceptQuest, 0.0f, 0.0f, 0.0f, questGiverEntry, 100.0f, questId, 0 },
                // Search radius kept tighter here -- see KNOWN_FAILURES.md #3.
                { AutonomousPlayer::GuideRuntime::StepType::KillNearest, 0.0f, 0.0f, 0.0f, killEntry, 50.0f, 0, 0 },
                { AutonomousPlayer::GuideRuntime::StepType::TurnInQuest, 0.0f, 0.0f, 0.0f, turnInEntry, 100.0f, questId, rewardChoiceIndex },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a full accept->kill->turn-in guide for quest {} for '{}'. No further commands "
                "needed -- check `.autonomousplayer guidestatus {}` to watch it advance on its own.",
                questId, charName, charName);
            return true;
        }

        // .autonomousplayer guidestartquestgrind <charname> <questId> <questGiverEntry> <killEntry> <turnInEntry> <rewardChoiceIndex> <killX> <killY> <killZ>
        //
        // ADR-048: the first COMPLETE quest loop for a quest whose
        // objectives need more than one kill -- accept, walk to the
        // hunting ground, kill+loot repeatedly until the engine's own
        // `CanCompleteQuest` says the objectives are met (collection
        // quests fill through the ordinary loot autostore, kill quests
        // through ordinary kill credit), then walk back and turn in.
        // The turn-in step gets a wider search radius (150yd) than
        // `guidestartquest`'s, because the bot ends the grind wherever
        // the last kill happened, not next to the giver.
        static bool HandleGuideStartQuestGrindCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartquestgrind <charname> <questId> <questGiverEntry> "
                    "<killEntry> <turnInEntry> <rewardChoiceIndex> <killX> <killY> <killZ>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 questId = 0, questGiverEntry = 0, killEntry = 0, turnInEntry = 0, rewardChoiceIndex = 0;
            float killX = 0.0f, killY = 0.0f, killZ = 0.0f;

            if (!(stream >> charName >> questId >> questGiverEntry >> killEntry >> turnInEntry
                    >> rewardChoiceIndex >> killX >> killY >> killZ))
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartquestgrind <charname> <questId> <questGiverEntry> "
                    "<killEntry> <turnInEntry> <rewardChoiceIndex> <killX> <killY> <killZ> [opportunisticSpellId]");
                return false;
            }

            // Optional trailing spells: before these existed, quest
            // grinds fought with bare melee only -- the ADR-029 "class
            // controller" composition was plumbed for guidestartcombat
            // but never for the quest loop, live-observed as slow,
            // death-prone even-level fights on the 1->12 run. The
            // second trailing id is the ADR-053 self-heal.
            uint32 opportunisticSpellId = 0, selfHealSpellId = 0;
            stream >> opportunisticSpellId >> selfHealSpellId;

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::AcceptQuest, 0.0f, 0.0f, 0.0f, questGiverEntry, 100.0f, questId, 0 },
                { AutonomousPlayer::GuideRuntime::StepType::MoveTo, killX, killY, killZ },
                { AutonomousPlayer::GuideRuntime::StepType::KillNearest, 0.0f, 0.0f, 0.0f, killEntry, 50.0f, questId, 0, opportunisticSpellId, true, 0, selfHealSpellId },
                { AutonomousPlayer::GuideRuntime::StepType::TurnInQuest, 0.0f, 0.0f, 0.0f, turnInEntry, 150.0f, questId, rewardChoiceIndex },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a full accept->grind-until-complete->turn-in guide for quest {} for '{}'. "
                "No further commands needed -- check `.autonomousplayer guidestatus {}` to watch it.",
                questId, charName, charName);
            return true;
        }

        // .autonomousplayer guidestartselljunk <charname> <vendorEntry> <vendorX> <vendorY> <vendorZ>
        //
        // KNOWN_FAILURES.md #29's durable fix, composed the same way as
        // guidestartquestgrind: MoveTo the vendor's vicinity, then a
        // SellJunk step walks to the nearest <vendorEntry> within 100yd
        // and sells every gray item, finishing only when a re-count
        // reads zero. Idempotent end to end -- re-issuing after a
        // bounded failure (or with nothing to sell) resumes/no-ops.
        static bool HandleGuideStartSellJunkCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartselljunk <charname> <vendorEntry> "
                    "<vendorX> <vendorY> <vendorZ>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 vendorEntry = 0;
            float vendorX = 0.0f, vendorY = 0.0f, vendorZ = 0.0f;

            if (!(stream >> charName >> vendorEntry >> vendorX >> vendorY >> vendorZ))
            {
                handler->SendSysMessage(
                    "Usage: .autonomousplayer guidestartselljunk <charname> <vendorEntry> "
                    "<vendorX> <vendorY> <vendorZ>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps
            {
                { AutonomousPlayer::GuideRuntime::StepType::MoveTo, vendorX, vendorY, vendorZ },
                { AutonomousPlayer::GuideRuntime::StepType::SellJunk, 0.0f, 0.0f, 0.0f, vendorEntry, 100.0f },
            };

            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage(
                "Started a sell-junk guide for '{}' at vendor entry {}. "
                "Check `.autonomousplayer guidestatus {}` -- grayItems reaching 0 is the success signal.",
                charName, vendorEntry, charName);
            return true;
        }

        static bool StartQuestBehaviorGuide(ChatHandler* handler, std::string const& charName,
            AutonomousPlayer::GuideRuntime::GuideStep step, char const* label)
        {
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            if (guid.IsEmpty() || !sBotLifecycleMgr->IsRegistered(guid))
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }
            std::vector<AutonomousPlayer::GuideRuntime::GuideStep> steps;
            steps.push_back(step);
            sBotLifecycleMgr->StartGuide(guid, std::move(steps));
            handler->PSendSysMessage("Started {} quest behavior for '{}'.", label, charName);
            return true;
        }

        static bool HandleGuideStartGameObjectCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0, gameObjectEntry = 0;
            float radius = 75.0f;
            if (!(stream >> charName >> questId >> gameObjectEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartgameobject <char> <quest> <goEntry> [radius]");
                return false;
            }
            stream >> radius;
            AutonomousPlayer::GuideRuntime::GuideStep step;
            step.Type = AutonomousPlayer::GuideRuntime::StepType::InteractGameObject;
            step.QuestId = questId;
            step.GameObjectEntry = gameObjectEntry;
            step.SearchRadius = radius;
            return StartQuestBehaviorGuide(handler, charName, step, "gameobject interaction");
        }

        static bool HandleGuideStartUseItemUnitCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0, itemId = 0, targetEntry = 0;
            float radius = 75.0f;
            if (!(stream >> charName >> questId >> itemId >> targetEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartuseitemunit <char> <quest> <item> <creature> [radius]");
                return false;
            }
            stream >> radius;
            AutonomousPlayer::GuideRuntime::GuideStep step;
            step.Type = AutonomousPlayer::GuideRuntime::StepType::UseItemOnUnit;
            step.QuestId = questId;
            step.ItemId = itemId;
            step.TargetEntry = targetEntry;
            step.SearchRadius = radius;
            return StartQuestBehaviorGuide(handler, charName, step, "use-item-on-unit");
        }

        static bool HandleGuideStartUseItemLocationCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0, itemId = 0;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (!(stream >> charName >> questId >> itemId >> x >> y >> z))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartuseitemlocation <char> <quest> <item> <x> <y> <z>");
                return false;
            }
            AutonomousPlayer::GuideRuntime::GuideStep step;
            step.Type = AutonomousPlayer::GuideRuntime::StepType::UseItemAtLocation;
            step.QuestId = questId;
            step.ItemId = itemId;
            step.X = x; step.Y = y; step.Z = z;
            return StartQuestBehaviorGuide(handler, charName, step, "use-item-at-location");
        }

        static bool HandleGuideStartAreaTriggerCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0, areaTriggerId = 0;
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (!(stream >> charName >> questId >> areaTriggerId >> x >> y >> z))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestartareatrigger <char> <quest> <trigger> <x> <y> <z>");
                return false;
            }
            AutonomousPlayer::GuideRuntime::GuideStep step;
            step.Type = AutonomousPlayer::GuideRuntime::StepType::ExploreAreaTrigger;
            step.QuestId = questId;
            step.AreaTriggerId = areaTriggerId;
            step.X = x; step.Y = y; step.Z = z;
            return StartQuestBehaviorGuide(handler, charName, step, "area-trigger exploration");
        }

        // .autonomousplayer guidestarttransport <char> <transportEntry>
        //   <waitX> <waitY> <waitZ> <standX> <standY> <standZ>
        //   <endX> <endY> <endZ> <getoffX> <getoffY> <getoffZ>
        // Board a zeppelin/boat at the dock (WaitAt), ride it, get off at the
        // destination -- the transport_routes.json legs map 1:1 to these args.
        static bool HandleGuideStartTransportCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 entry = 0;
            AutonomousPlayer::GuideRuntime::GuideStep step;
            step.Type = AutonomousPlayer::GuideRuntime::StepType::UseTransport;
            if (!(stream >> charName >> entry
                    >> step.X >> step.Y >> step.Z
                    >> step.TransportStartX >> step.TransportStartY >> step.TransportStartZ
                    >> step.StandOnX >> step.StandOnY >> step.StandOnZ
                    >> step.TransportEndX >> step.TransportEndY >> step.TransportEndZ
                    >> step.GetOffX >> step.GetOffY >> step.GetOffZ))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestarttransport <char> <transportEntry> "
                    "<waitX> <waitY> <waitZ> <startX> <startY> <startZ> <standX> <standY> <standZ> "
                    "<endX> <endY> <endZ> <getoffX> <getoffY> <getoffZ>");
                return false;
            }
            step.TransportEntry = entry;
            return StartQuestBehaviorGuide(handler, charName, step, "transport ride");
        }

        // .autonomousplayer guidestatus <charname>
        // .autonomousplayer threats <charname> <radius>
        //
        // Live hostiles around the bot: every alive creature the engine
        // itself considers unfriendly to this player (real faction
        // hostility -- not the offline faction proxy the threat snapshot
        // uses), with entry/level/position. Consumed by route_runner's
        // transit planner as dynamic threat points, the same idea as
        // HonorBuddy's AvoidanceManager (live mobs become temporary
        // blackspots) adapted to our external-runner architecture.
        static bool HandleThreatsCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            float radius = 0.f;
            if (!(stream >> charName >> radius))
            {
                handler->SendSysMessage("Usage: .autonomousplayer threats <charname> <radius>");
                return false;
            }
            radius = std::min(std::max(radius, 10.f), 300.f);

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            std::list<Unit*> targets;
            Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, radius);
            Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, targets, check);
            Cell::VisitObjects(player, searcher, radius);

            uint32 printed = 0;
            for (Unit* unit : targets)
            {
                if (!unit->IsCreature() || !unit->IsAlive())
                    continue;
                if (printed >= 80)
                    break;
                handler->PSendSysMessage("threat entry={} level={} pos=({:.1f}, {:.1f}, {:.1f})",
                    unit->GetEntry(), unit->GetLevel(), unit->GetPositionX(),
                    unit->GetPositionY(), unit->GetPositionZ());
                ++printed;
            }
            handler->PSendSysMessage("threats: {} hostile creature(s) within {:.0f}yd of '{}'.",
                printed, radius, charName);
            return true;
        }

        static bool HandleGuideStatusCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestatus <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            if (!(stream >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer guidestatus <charname>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            auto const* state = guid.IsEmpty() ? nullptr : sBotLifecycleMgr->GetGuideState(guid);
            if (!state)
            {
                handler->PSendSysMessage("'{}' is not a registered bot.", charName);
                return true;
            }

            handler->PSendSysMessage(
                "Guide status for '{}': step {}/{}, action issued={}, finished={}, failed={}, phase={}, "
                "pullState={}, approachTicks={}, operationTicks={}, blacklisted={}, lastLootAttempted={}, "
                "lastLootVerified={}",
                charName, state->CurrentStep, state->Steps.size(),
                state->ActionIssuedForCurrentStep, state->Finished, state->Failed,
                static_cast<uint32>(state->CurrentPhase), static_cast<uint32>(state->CurrentPullState),
                state->ApproachTicks, state->OperationTicks, state->BlacklistedTargets.size(),
                state->LastLootAttempted, state->LastLootVerified);

            // #29 diagnosability: an engine-refused turn-in (full bags
            // for a reward item, most commonly) used to be
            // indistinguishable from a generic timeout here -- the
            // refusal only ever goes to the headless client session.
            // Bag numbers shown alongside so the full-bags case is
            // readable at a glance.
            if (Player* player = ObjectAccessor::FindPlayer(guid))
            {
                handler->PSendSysMessage(
                    "  inventory: turnInEngineRefused={} grayItems={} freeBagSlots={}",
                    state->TurnInEngineRefused,
                    AutonomousPlayer::Economy::CountSellableGrayItems(player),
                    AutonomousPlayer::Economy::CountFreeBagSlots(player));
            }

            // Per-rejection-reason breakdown of the most recent
            // KillNearest selection sweep (KNOWN_FAILURES.md #21's
            // diagnosability note): distinguishes "nothing in range at
            // all" (candidates=0) from "candidates found but all unsafe"
            // -- and names the dominant rejection reason directly --
            // instead of every drought producing the same bare
            // pullState=0 signature.
            handler->PSendSysMessage(
                "  selection: candidates={} dead={} blacklisted={} evading={} notAttackable={} "
                "tapped={} otherPlayerAttacking={} noLos={}",
                state->LastSelection.Candidates, state->LastSelection.Dead,
                state->LastSelection.Blacklisted, state->LastSelection.Evading,
                state->LastSelection.NotAttackable, state->LastSelection.Tapped,
                state->LastSelection.OtherPlayerAttacking, state->LastSelection.NoLineOfSight);
            handler->PSendSysMessage(
                "  readiness: ready={} reason={} hp={:.1f}% resource={:.1f}% usesMana={} "
                "pet={} petHp={:.1f}% durability={:.1f}% sickness={} attackers={} nearby={} "
                "safeRest={} foodDrink={}",
                state->LastReadiness.Ready, static_cast<uint32>(state->LastReadiness.BlockingReason),
                state->LastReadiness.HealthPct, state->LastReadiness.ResourcePct,
                state->LastReadiness.UsesMana, state->LastReadiness.HasActivePet,
                state->LastReadiness.PetHealthPct, state->LastReadiness.MinEquippedDurabilityPct,
                state->LastReadiness.HasResurrectionSickness,
                state->LastReadiness.CurrentAttackers, state->LastReadiness.NearbyAttackable,
                state->LastReadiness.SafeToRest, state->LastReadiness.FoodDrinkCount);
            handler->PSendSysMessage(
                "  risk: score={:.1f} levelDelta={} nearby={} mixedEntry={} corridor={} casters={} "
                "elites={} objective={} escape={} forcedDefense={} riskRejected={} locationBlacklisted={}",
                state->LastRisk.Score, state->LastRisk.LevelDelta, state->LastRisk.NearbyAttackable,
                state->LastRisk.MixedEntryAdds, state->LastRisk.CorridorThreats,
                state->LastRisk.CasterThreats, state->LastRisk.EliteThreats,
                state->LastRisk.ObjectiveRelevant, state->LastRisk.EscapePathAvailable,
                state->LastRisk.ForcedDefense, state->LastSelection.RiskRejected,
                state->LastSelection.LocationBlacklisted);
            handler->PSendSysMessage(
                "  pull: failureReason={} ticks={} outgoingDamage={} targetHpDelta={} "
                "incomingDamage={} botHpDelta={} unchangedTargetHpTicks={} targetBlacklist={} locationBlacklist={}",
                static_cast<uint32>(state->LastFailureReason), state->PullTicks,
                state->OutgoingDamage, state->TargetHealthDelta, state->IncomingDamage,
                state->BotHealthDelta, state->UnchangedTargetHealthTicks,
                state->BlacklistedTargets.size(), state->BlacklistedLocations.size());
            handler->PSendSysMessage(
                "  questAction: progress={} attempts={} unchangedTicks={} initialized={}",
                state->QuestProgress, state->InteractionAttempts,
                state->UnchangedQuestProgressTicks, state->QuestProgressInitialized);

            // Real diagnostics for the current interaction target (if
            // any) -- added to distinguish a genuine stall from slow but
            // real progress, rather than inferring purely from bot
            // position snapshots (see KNOWN_FAILURES.md #3).
            if (!state->CurrentTargetGuid.IsEmpty())
            {
                Player* player = ObjectAccessor::FindPlayer(guid);
                Creature* target = player ? ObjectAccessor::GetCreature(*player, state->CurrentTargetGuid) : nullptr;
                if (target && player)
                {
                    handler->PSendSysMessage(
                        "  target: '{}' alive={} pos ({:.1f}, {:.1f}, {:.1f}) distance={:.1f}",
                        target->GetName(), target->IsAlive(),
                        target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                        player->GetDistance(target));
                }
                else
                {
                    handler->PSendSysMessage("  target: guid {} does not resolve (despawned/out of range/wrong map).",
                        state->CurrentTargetGuid.ToString());
                }
            }

            // EncounterModel (ADR-024, Gate 3 implementation sequence
            // step 2): real, authoritative attacker awareness, not just
            // the guide's own single objective target.
            if (Player* player = ObjectAccessor::FindPlayer(guid))
            {
                AutonomousPlayer::EncounterModel::Snapshot snapshot =
                    AutonomousPlayer::EncounterModel::BuildSnapshot(player, state->CurrentTargetGuid);
                handler->PSendSysMessage(
                    "  encounter: botInCombat={} attackers={} hasUnplannedAdd={}",
                    snapshot.BotInCombat, snapshot.Attackers.size(), snapshot.HasUnplannedAdd());
                for (auto const& attacker : snapshot.Attackers)
                {
                    handler->PSendSysMessage(
                        "    attacker: entry={} distance={:.1f} isObjectiveTarget={}",
                        attacker.Entry, attacker.Distance, attacker.IsObjectiveTarget);
                }
            }

            return true;
        }

        // .autonomousplayer encountersnapshot <charname>
        //
        // Standalone EncounterModel diagnostic, independent of any
        // running guide -- useful for verifying multi-attacker detection
        // directly (e.g. after a manual `.autonomousplayer multipull`).
        static bool HandleEncounterSnapshotCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer encountersnapshot <charname>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            if (!(stream >> charName))
            {
                handler->SendSysMessage("Usage: .autonomousplayer encountersnapshot <charname>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            AutonomousPlayer::EncounterModel::Snapshot snapshot =
                AutonomousPlayer::EncounterModel::BuildSnapshot(player, ObjectGuid::Empty);
            handler->PSendSysMessage(
                "Encounter snapshot for '{}': botInCombat={} attackers={} hasUnplannedAdd={}",
                charName, snapshot.BotInCombat, snapshot.Attackers.size(), snapshot.HasUnplannedAdd());
            for (auto const& attacker : snapshot.Attackers)
            {
                handler->PSendSysMessage(
                    "  attacker: guid={} entry={} distance={:.1f}",
                    attacker.Guid.ToString(), attacker.Entry, attacker.Distance);
            }

            return true;
        }

        // .autonomousplayer tamebeast <charname> <creatureEntry>
        //
        // Gate 3 pets first slice (ADR-037): walks to the nearest live
        // creature of `creatureEntry` (real navmesh movement, same as
        // every other targeted debug command in this module) and casts
        // real Tame Beast (Pets::TameBeastSpellId) at it. This only
        // starts the cast -- Tame Beast has a real cast time, so the pet
        // does not exist yet when this command returns. Poll
        // `.autonomousplayer petstatus` to watch it complete.
        static bool HandleTameBeastCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer tamebeast <charname> <creatureEntry>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 creatureEntry = 0;

            if (!(stream >> charName >> creatureEntry))
            {
                handler->SendSysMessage("Usage: .autonomousplayer tamebeast <charname> <creatureEntry>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            Creature* target = player->FindNearestCreature(creatureEntry, 100.0f, true);
            if (!target)
            {
                handler->PSendSysMessage("No live creature with entry {} within 100 yards of '{}'.",
                    creatureEntry, charName);
                return true;
            }

            // Same real-range check as HandleCastSpellCommand's ADR-036
            // fix -- move only if actually out of Tame Beast's own range,
            // so a retry against an already-in-range target doesn't spuriously
            // fail with SPELL_FAILED_MOVING.
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(AutonomousPlayer::Pets::TameBeastSpellId);
            float maxRange = spellInfo ? spellInfo->GetMaxRange(true, player) : 0.0f;
            if (maxRange <= 0.0f || player->GetDistance(target) > maxRange)
            {
                AutonomousPlayer::Navigation::MoveTo(
                    player, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
            }

            SpellCastResult result = AutonomousPlayer::Pets::RequestTameBeast(player, target);
            handler->PSendSysMessage(
                "Tame Beast at '{}' by '{}': result={} ({}). Poll `.autonomousplayer petstatus {}` "
                "to watch the real cast time complete.",
                target->GetName(), charName, static_cast<uint32>(result),
                result == SPELL_CAST_OK ? "SPELL_CAST_OK" : "rejected", charName);
            return true;
        }

        // .autonomousplayer petstatus <charname>
        static bool HandlePetStatusCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer petstatus <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            AutonomousPlayer::Pets::PetSnapshot snapshot = AutonomousPlayer::Pets::BuildSnapshot(player);
            if (!snapshot.HasPet)
            {
                // KNOWN_FAILURES.md #13 diagnostic: GetPet()==null is
                // reported above via BuildSnapshot, but that alone can't
                // tell a clean "no pet" apart from a stale summon-slot
                // guid or a real, recoverable stable entry -- report the
                // full real classification so this doesn't have to be
                // reverse-engineered from behavior again. `lastKnownGuid`
                // isn't tracked by this debug command (only
                // `GuideRuntime::BotGuideState` has it), so `NoPet` and
                // `Dismissed` are indistinguishable here -- that's
                // expected and fine for a diagnostic command.
                AutonomousPlayer::Pets::PetState state = AutonomousPlayer::Pets::ClassifyPetState(
                    player, snapshot, ObjectGuid::Empty);
                char const* stateName = "unknown";
                switch (state)
                {
                    case AutonomousPlayer::Pets::PetState::NoPet: stateName = "NoPet/Dismissed"; break;
                    case AutonomousPlayer::Pets::PetState::MissingAlive: stateName = "MissingAlive"; break;
                    case AutonomousPlayer::Pets::PetState::MissingDead: stateName = "MissingDead"; break;
                    default: break;
                }
                handler->PSendSysMessage("'{}' has no pet. state={} rawPetGuid={} staleSlot={}",
                    charName, stateName,
                    player->GetPetGUID().IsEmpty() ? "empty" : player->GetPetGUID().ToString(),
                    AutonomousPlayer::Pets::HasStalePetSlot(player));
                return true;
            }

            handler->PSendSysMessage(
                "Pet for '{}': guid={} entry={} alive={} hp={}/{} reactState={} victim={}",
                charName, snapshot.Guid.ToString(), snapshot.Entry, snapshot.Alive,
                snapshot.Health, snapshot.MaxHealth, static_cast<uint32>(snapshot.React),
                snapshot.VictimGuid.IsEmpty() ? "none" : snapshot.VictimGuid.ToString());
            return true;
        }

        // .autonomousplayer petreactstate <charname> <0|1|2>
        //
        // 0=passive (real default on a freshly tamed pet, confirmed
        // live -- it will NOT auto-assist in combat until commanded),
        // 1=defensive, 2=aggressive.
        static bool HandlePetReactStateCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer petreactstate <charname> <0=passive|1=defensive|2=aggressive>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 state = 0;

            if (!(stream >> charName >> state) || state > REACT_AGGRESSIVE)
            {
                handler->SendSysMessage("Usage: .autonomousplayer petreactstate <charname> <0=passive|1=defensive|2=aggressive>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool ok = AutonomousPlayer::Pets::RequestSetPetReactState(player, static_cast<ReactStates>(state));
            handler->PSendSysMessage("Set pet react state to {} for '{}': submitted={}", state, charName, ok);
            return true;
        }

        // .autonomousplayer revivepet <charname>
        //
        // Deliberately does NOT go through the generic `castspell`
        // debug command -- Revive Pet is self-targeted (its real effect,
        // `Spell::EffectResurrectPet`, acts on `player->GetPet()`
        // internally; there is no meaningful external unit target), and
        // `castspell`'s own out-of-range-then-move logic (ADR-036) makes
        // no sense against an unrelated dummy target for a self-cast
        // spell -- confirmed live: it kept re-triggering
        // `SPELL_FAILED_MOVING` every invocation, the exact same class
        // of debug-tooling artifact ADR-036 fixed for a different case.
        // This command calls the real primitive directly instead.
        static bool HandleRevivePetCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer revivepet <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            SpellCastResult result = AutonomousPlayer::Pets::RequestRevivePet(player);
            handler->PSendSysMessage(
                "Revive Pet by '{}': result={} ({}). Poll `.autonomousplayer petstatus {}` to check.",
                charName, static_cast<uint32>(result), result == SPELL_CAST_OK ? "SPELL_CAST_OK" : "rejected",
                charName);
            return true;
        }

        // .autonomousplayer dismisspet <charname>
        //
        // Test/debug tooling -- casts the real Dismiss Pet spell (2641,
        // `Spell::EffectDismissPet` -> `pet->Remove(PET_SAVE_NOT_IN_SLOT)`),
        // the recoverable unslot a real Hunter's "Dismiss Pet" ability
        // performs. This is the real way to construct
        // `Pets::PetState::MissingAlive` for testing
        // `Recovery::PlanPetRecovery`'s `CallPet` branch
        // (`KNOWN_FAILURES.md` #17 -- `abandonpet` below permanently
        // deletes instead). Same direct-primitive style as `revivepet`
        // (see that command's comment for why not generic `castspell`).
        static bool HandleDismissPetCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer dismisspet <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            SpellCastResult result = AutonomousPlayer::Pets::RequestDismissPet(player);
            handler->PSendSysMessage(
                "Dismiss Pet by '{}': result={} ({}). Poll `.autonomousplayer petstatus {}` to check.",
                charName, static_cast<uint32>(result), result == SPELL_CAST_OK ? "SPELL_CAST_OK" : "rejected",
                charName);
            return true;
        }

        // .autonomousplayer abandonpet <charname>
        //
        // Test/debug tooling only -- **permanently deletes** the pet via
        // the real CMSG_PET_ABANDON opcode handler
        // (`WorldSession::HandlePetAbandon` -> `PET_SAVE_AS_DELETED`,
        // confirmed live: the `character_pet` row is gone afterward,
        // `KNOWN_FAILURES.md` #17), the same action a real player's
        // "abandon pet" confirmation sends. NOT a recoverable dismiss --
        // use `dismisspet` above for that. No guide step or recovery
        // policy calls this.
        static bool HandleAbandonPetCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer abandonpet <charname>");
                return false;
            }

            std::string charName(args);
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            bool submitted = AutonomousPlayer::Pets::RequestAbandonPet(player);
            handler->PSendSysMessage(
                "Abandon Pet by '{}': submitted={}. Poll `.autonomousplayer petstatus {}` to check.",
                charName, submitted, charName);
            return true;
        }

        // .autonomousplayer queststatus <charname> <questId>
        static bool HandleQuestStatusCommand(ChatHandler* handler, char const* args)
        {
            if (!args || !*args)
            {
                handler->SendSysMessage("Usage: .autonomousplayer queststatus <charname> <questId>");
                return false;
            }

            std::istringstream stream(args);
            std::string charName;
            uint32 questId = 0;

            if (!(stream >> charName >> questId))
            {
                handler->SendSysMessage("Usage: .autonomousplayer queststatus <charname> <questId>");
                return false;
            }

            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }

            // canComplete: objectives are met even if the status flag
            // never flipped to COMPLETE. Kill quests credited via the
            // engine grind (esp. multi-objective ones) were observed
            // stuck at INCOMPLETE with all mob counts met, so the
            // orchestrator's turn-in branch never fired -- expose the
            // real "objectives done" signal so it can complete them.
            bool canComplete = player->CanCompleteQuest(questId);
            handler->PSendSysMessage(
                "Quest {} status for '{}': {} (rewarded={}) lvl={} xp={} canComplete={}",
                questId, charName, static_cast<int>(player->GetQuestStatus(questId)),
                player->IsQuestRewarded(questId), player->GetLevel(),
                player->GetUInt32Value(PLAYER_XP), canComplete);
            return true;
        }

        // .autonomousplayer completequest <charname> <questId>
        //
        // Flip an objectives-met quest to COMPLETE (Player::CompleteQuest)
        // WITHOUT rewarding, so the bot then turns it in at the real NPC
        // normally. The honest fix for kill quests whose status never
        // auto-flipped despite full mob credit (see queststatus
        // canComplete). No-op unless CanCompleteQuest is true.
        static bool HandleCompleteQuestCommand(ChatHandler* handler, char const* args)
        {
            std::istringstream stream(args ? args : "");
            std::string charName;
            uint32 questId = 0;
            if (!(stream >> charName >> questId))
            {
                handler->SendSysMessage("Usage: .autonomousplayer completequest <charname> <questId>");
                return false;
            }
            ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(charName);
            Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                handler->PSendSysMessage("'{}' is not online.", charName);
                return true;
            }
            if (!player->CanCompleteQuest(questId))
            {
                handler->PSendSysMessage(
                    "Quest {} for '{}': objectives NOT met -- not completing.", questId, charName);
                return true;
            }
            player->CompleteQuest(questId);
            handler->PSendSysMessage(
                "Quest {} for '{}': flipped to COMPLETE (status {}) -- turn in at the NPC.",
                questId, charName, static_cast<int>(player->GetQuestStatus(questId)));
            return true;
        }

        // .autonomousplayer status
        // .autonomousplayer status [charname]
        //
        // With a name, reports only that bot. The dump-everything
        // default made every caller's regex a crossfeed hazard: one
        // orchestrator parsed the FIRST corpse line of the full dump
        // as its own bot's corpse and marched ghosts fleet-wide toward
        // another bot's death site.
        static bool HandleStatusCommand(ChatHandler* handler, char const* args)
        {
            std::string filter = args ? args : "";
            while (!filter.empty() && filter.back() == ' ')
            {
                filter.pop_back();
            }
            if (filter == "all")
            {
                filter.clear();
            }

            std::vector<ObjectGuid> guids = sBotLifecycleMgr->GetRegisteredBotGuids();

            if (filter.empty())
            {
                handler->PSendSysMessage("mod-autonomous-player: {} bot(s) registered.", guids.size());
            }

            for (ObjectGuid const& guid : guids)
            {
                Player* player = ObjectAccessor::FindPlayer(guid);
                if (!player)
                {
                    if (filter.empty())
                    {
                        handler->PSendSysMessage("  {} - registered but not resolvable this tick.", guid.ToString());
                    }
                    continue;
                }

                if (!filter.empty() && player->GetName() != filter)
                {
                    continue;
                }

                AutonomousPlayer::PerceptionSnapshot snapshot =
                    AutonomousPlayer::BuildPerceptionSnapshot(player);

                // Bag capacity: backpack (16) + every equipped bag.
                uint32 bagTotal = 16, bagFree = 0;
                for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                {
                    if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    {
                        ++bagFree;
                    }
                }
                for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                {
                    if (Bag* bag = player->GetBagByPos(b))
                    {
                        bagTotal += bag->GetBagSize();
                        bagFree += bag->GetFreeSlots();
                    }
                }
                uint32 bagUsed = bagTotal - bagFree;

                // Average equipped item level.
                uint32 ilvlSum = 0, ilvlCount = 0;
                for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
                {
                    if (Item* it = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                    {
                        ilvlSum += it->GetTemplate()->ItemLevel;
                        ++ilvlCount;
                    }
                }
                uint32 ilvl = ilvlCount ? ilvlSum / ilvlCount : 0;

                handler->PSendSysMessage(
                    "  {} lvl {} map {} pos ({:.1f}, {:.1f}, {:.1f}) hp {}/{} alive={} combat={} ghost={}"
                    " bags {}/{} ilvl {} quests {}",
                    snapshot.CharacterName, snapshot.Level, snapshot.MapId,
                    snapshot.PositionX, snapshot.PositionY, snapshot.PositionZ,
                    snapshot.Health, snapshot.MaxHealth, snapshot.IsAlive, snapshot.IsInCombat,
                    snapshot.IsGhost, bagUsed, bagTotal, ilvl,
                    static_cast<uint32>(player->GetRewardedQuestCount()));

                if (snapshot.HasCorpse)
                {
                    handler->PSendSysMessage(
                        "    corpse at ({:.1f}, {:.1f}, {:.1f})",
                        snapshot.CorpseX, snapshot.CorpseY, snapshot.CorpseZ);
                }
            }

            return true;
        }
    };
} // namespace

void AddSC_autonomousplayer_commandscript()
{
    new AutonomousPlayerCommandScript();
}
