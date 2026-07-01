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
#include "CharacterCache.h"
#include "Combat/BotCombat.h"
#include "CommandScript.h"
#include "Common.h"
#include "Creature.h"
#include "Economy/BotEconomy.h"
#include "EncounterModel/BotEncounterModel.h"
#include "GossipDef.h"
#include "Gossip/BotGossip.h"
#include "Growth/BotGrowth.h"
#include "GuideRuntime/BotGuideRuntime.h"
#include "Inventory/BotLoot.h"
#include "Lifecycle/BotLifecycleMgr.h"
#include "Lifecycle/BotLogin.h"
#include "Lifecycle/BotSessionMgr.h"
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "Perception/PerceptionBuilder.h"
#include "Player.h"
#include "QuestEngine/BotQuestEngine.h"
#include "Recovery/BotRecovery.h"
#include "Setup/BotProvisioning.h"
#include "Setup/PendingCharacterCreations.h"

#include <cstdlib>
#include <list>
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
                { "status",    HandleStatusCommand,    SEC_GAMEMASTER,    Console::Yes },
                { "moveto",    HandleMoveToCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "acceptquest", HandleAcceptQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "queststatus", HandleQuestStatusCommand, SEC_GAMEMASTER,    Console::Yes },
                { "turnin",    HandleTurnInCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "attack",    HandleAttackCommand,    SEC_ADMINISTRATOR, Console::Yes },
                { "creaturestatus", HandleCreatureStatusCommand, SEC_GAMEMASTER, Console::Yes },
                { "targetsafety", HandleTargetSafetyCommand, SEC_GAMEMASTER, Console::Yes },
                { "loot",      HandleLootCommand,      SEC_ADMINISTRATOR, Console::Yes },
                { "releasespirit", HandleReleaseSpiritCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "reclaimcorpse", HandleReclaimCorpseCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "attackguid", HandleAttackGuidCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "multipull", HandleMultiPullCommand,  SEC_ADMINISTRATOR, Console::Yes },
                { "buy",       HandleBuyCommand,        SEC_ADMINISTRATOR, Console::Yes },
                { "repair",    HandleRepairCommand,     SEC_ADMINISTRATOR, Console::Yes },
                { "gossiphello", HandleGossipHelloCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "gossiptrain", HandleGossipTrainCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "learnspell", HandleLearnSpellCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "castspell", HandleCastSpellCommand,  SEC_ADMINISTRATOR, Console::Yes },
                { "spellbook", HandleSpellbookCommand,  SEC_GAMEMASTER,    Console::Yes },
                { "guidestart", HandleGuideStartCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartmoveto", HandleGuideStartMoveToCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartcombat", HandleGuideStartCombatCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartcombatability", HandleGuideStartCombatAbilityCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestartquest", HandleGuideStartQuestCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "guidestatus", HandleGuideStatusCommand, SEC_GAMEMASTER, Console::Yes },
                { "encountersnapshot", HandleEncounterSnapshotCommand, SEC_GAMEMASTER, Console::Yes },
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

            Creature* target = player->FindNearestCreature(creatureEntry, 100.0f, false);
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
            bool los = player->IsWithinLOSInMap(target);

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

            AutonomousPlayer::Navigation::MoveTo(player, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());

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

        // .autonomousplayer guidestatus <charname>
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

            handler->PSendSysMessage("Quest {} status for '{}': {} (rewarded={}) lvl={} xp={}",
                questId, charName, static_cast<int>(player->GetQuestStatus(questId)),
                player->IsQuestRewarded(questId), player->GetLevel(), player->GetUInt32Value(PLAYER_XP));
            return true;
        }

        // .autonomousplayer status
        static bool HandleStatusCommand(ChatHandler* handler, char const* /*args*/)
        {
            std::vector<ObjectGuid> guids = sBotLifecycleMgr->GetRegisteredBotGuids();

            handler->PSendSysMessage("mod-autonomous-player: {} bot(s) registered.", guids.size());

            for (ObjectGuid const& guid : guids)
            {
                Player* player = ObjectAccessor::FindPlayer(guid);
                if (!player)
                {
                    handler->PSendSysMessage("  {} - registered but not resolvable this tick.", guid.ToString());
                    continue;
                }

                AutonomousPlayer::PerceptionSnapshot snapshot =
                    AutonomousPlayer::BuildPerceptionSnapshot(player);

                handler->PSendSysMessage(
                    "  {} lvl {} map {} pos ({:.1f}, {:.1f}, {:.1f}) hp {}/{} alive={} combat={} ghost={}",
                    snapshot.CharacterName, snapshot.Level, snapshot.MapId,
                    snapshot.PositionX, snapshot.PositionY, snapshot.PositionZ,
                    snapshot.Health, snapshot.MaxHealth, snapshot.IsAlive, snapshot.IsInCombat,
                    snapshot.IsGhost);

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
