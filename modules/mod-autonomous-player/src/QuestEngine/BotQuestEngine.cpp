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

#include "BotQuestEngine.h"
#include "Object.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::QuestEngine
{
    bool RequestAcceptQuest(Player* bot, uint32_t questId, ObjectGuid const& questGiverGuid)
    {
        if (!bot || !bot->GetSession())
        {
            return false;
        }

        WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST, 16);
        packet << questGiverGuid;
        packet << uint32(questId);
        packet << uint32(0); // unk1 -- read but unused by the handler

        bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);

        return true;
    }
} // namespace AutonomousPlayer::QuestEngine
