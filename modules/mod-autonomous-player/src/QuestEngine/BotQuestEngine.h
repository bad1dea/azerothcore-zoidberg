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

#ifndef AUTONOMOUS_PLAYER_BOT_QUEST_ENGINE_H
#define AUTONOMOUS_PLAYER_BOT_QUEST_ENGINE_H

#include <cstdint>

class ObjectGuid;
class Player;

namespace AutonomousPlayer::QuestEngine
{
    // Submits a real quest-accept request on `bot` via the same public
    // opcode handler (WorldSession::HandleQuestgiverAcceptQuestOpcode) a
    // game client uses when the player clicks "Accept" in the quest-giver
    // dialog. Unlike login/character creation (ADR-008), this handler is
    // fully synchronous -- no DB round trip, no BotSessionMgr survival
    // concerns -- so no special handling is needed beyond building the
    // packet and calling it.
    //
    // All of the real acceptance rules run exactly as they would for a
    // human player: prerequisite/exclusive-group/race-class/level checks
    // (CanTakeQuest), quest-log-full checks (CanAddQuest), distance/state
    // checks (CanInteractWithQuestGiver), and the quest-giver actually
    // offering this quest (Object::hasQuest). This function does not
    // duplicate any of that logic -- it only decides what to *ask for*.
    //
    // Returns true if the request was submitted (not a guarantee of
    // success -- the handler doesn't report failures back to a socketless
    // session, same caveat as ADR-008). Verify with
    // bot->GetQuestStatus(questId) != QUEST_STATUS_NONE afterward.
    bool RequestAcceptQuest(Player* bot, uint32_t questId, ObjectGuid const& questGiverGuid);
} // namespace AutonomousPlayer::QuestEngine

#endif // AUTONOMOUS_PLAYER_BOT_QUEST_ENGINE_H
