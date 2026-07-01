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

#ifndef AUTONOMOUS_PLAYER_BOT_LOGIN_H
#define AUTONOMOUS_PLAYER_BOT_LOGIN_H

#include <string>

namespace AutonomousPlayer::Lifecycle
{
    // Looks up `characterName` in the character cache and, if found and not
    // already online, creates a bot WorldSession for `accountName` (see
    // Setup::CreateBotSession) and submits a real login request through the
    // same public opcode handler a game client uses
    // (WorldSession::HandlePlayerLoginOpcode, fed a synthesized
    // CMSG_PLAYER_LOGIN packet -- see ADR-008). This is asynchronous: the
    // character is not guaranteed to be in the world by the time this
    // returns. Login completion is observed via the normal PlayerScript
    // login hook (see AutonomousPlayerModule.cpp), which registers the bot
    // with BotLifecycleMgr once the world confirms the login.
    //
    // Returns false immediately if the account or character doesn't exist,
    // or the character is already online -- true if a login request was
    // submitted (not yet necessarily completed).
    bool TryLoginBot(std::string const& accountName, std::string const& characterName);
} // namespace AutonomousPlayer::Lifecycle

#endif // AUTONOMOUS_PLAYER_BOT_LOGIN_H
