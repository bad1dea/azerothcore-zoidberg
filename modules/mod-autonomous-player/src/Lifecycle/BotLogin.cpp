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

#include "BotLogin.h"
#include "AccountMgr.h"
#include "BotSessionMgr.h"
#include "CharacterCache.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Setup/BotProvisioning.h"
#include "Telemetry/Telemetry.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Lifecycle
{
    bool TryLoginBot(std::string const& accountName, std::string const& characterName)
    {
        uint32 accountId = AccountMgr::GetId(accountName);
        if (!accountId)
        {
            LOG_ERROR(Telemetry::LogCategory, "TryLoginBot: account '{}' does not exist.", accountName);
            return false;
        }

        ObjectGuid characterGuid = sCharacterCache->GetCharacterGuidByName(characterName);
        if (characterGuid.IsEmpty())
        {
            LOG_ERROR(Telemetry::LogCategory, "TryLoginBot: character '{}' does not exist.", characterName);
            return false;
        }

        if (ObjectAccessor::FindPlayer(characterGuid))
        {
            LOG_INFO(Telemetry::LogCategory, "TryLoginBot: character '{}' is already online.", characterName);
            return false;
        }

        WorldSession* session = Setup::CreateBotSession(accountId, accountName);

        // Must be tracked BEFORE the login opcode call, and stays tracked
        // for the bot's entire online lifetime -- see ADR-008 and
        // BotSessionMgr's header comment. Untracked (and deleted) in
        // AutonomousPlayerModule.cpp's OnPlayerLogout hook.
        sBotSessionMgr->TrackSession(session);

        WorldPacket packet(CMSG_PLAYER_LOGIN, 8);
        packet << characterGuid;

        session->HandlePlayerLoginOpcode(packet);

        return true;
    }
} // namespace AutonomousPlayer::Lifecycle
