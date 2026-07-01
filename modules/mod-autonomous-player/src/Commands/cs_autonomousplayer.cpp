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
#include <sstream>

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
                { "loot",      HandleLootCommand,      SEC_ADMINISTRATOR, Console::Yes },
                { "releasespirit", HandleReleaseSpiritCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "reclaimcorpse", HandleReclaimCorpseCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "attackguid", HandleAttackGuidCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "multipull", HandleMultiPullCommand,  SEC_ADMINISTRATOR, Console::Yes },
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
