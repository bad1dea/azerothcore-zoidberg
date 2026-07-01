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
#include "CommandScript.h"
#include "Common.h"
#include "Creature.h"
#include "Lifecycle/BotLifecycleMgr.h"
#include "Lifecycle/BotLogin.h"
#include "Lifecycle/BotSessionMgr.h"
#include "Navigation/BotNavigation.h"
#include "ObjectAccessor.h"
#include "Perception/PerceptionBuilder.h"
#include "Player.h"
#include "QuestEngine/BotQuestEngine.h"
#include "Setup/BotProvisioning.h"
#include "Setup/PendingCharacterCreations.h"

#include <cstdlib>
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
                    "  {} lvl {} map {} pos ({:.1f}, {:.1f}, {:.1f}) hp {}/{} alive={} combat={}",
                    snapshot.CharacterName, snapshot.Level, snapshot.MapId,
                    snapshot.PositionX, snapshot.PositionY, snapshot.PositionZ,
                    snapshot.Health, snapshot.MaxHealth, snapshot.IsAlive, snapshot.IsInCombat);
            }

            return true;
        }
    };
} // namespace

void AddSC_autonomousplayer_commandscript()
{
    new AutonomousPlayerCommandScript();
}
