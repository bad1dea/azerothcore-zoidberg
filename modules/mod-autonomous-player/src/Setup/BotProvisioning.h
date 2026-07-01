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

#ifndef AUTONOMOUS_PLAYER_BOT_PROVISIONING_H
#define AUTONOMOUS_PLAYER_BOT_PROVISIONING_H

#include <cstdint>
#include <string>

class WorldSession;

namespace AutonomousPlayer::Setup
{
    // One-time/idempotent bot account+character provisioning. This is
    // explicitly the "test setup" side of the player-like policy (see
    // ARCHITECTURE.md ADR-005/ADR-008): it is only ever invoked from the
    // .autonomousplayer admin command (Commands/cs_autonomousplayer.cpp),
    // never from the tick-driven bot runtime loop. Every mutation here goes
    // through the same public, real AzerothCore APIs a human player or GM
    // would use -- AccountMgr::CreateAccount and the real
    // WorldSession::HandleCharCreateOpcode opcode handler (via a
    // synthesized CMSG_CHAR_CREATE packet) -- not direct DB writes.

    // Every bot-owning account name must start with this prefix.
    //
    // WorldSession::IsBot() is NOT exclusive to this module -- mod-playerbots
    // sets it on its own (potentially hundreds of) random-bot sessions too.
    // Discovered live on zoidberg in the Gate 1 session that added this: an
    // IsBot()-only login hook silently registered every Playerbots bot into
    // BotLifecycleMgr. Account-name-prefix ownership (checked via
    // IsAutonomousPlayerAccount, restart-safe since it's a DB lookup, not
    // in-memory state) is how this module tells "its own" bots apart from
    // any other system's bots that happen to share the same core flag.
    inline constexpr char const* AccountPrefix = "autonomous_player_";

    // True if `accountId` is a bot-owning account created by this module
    // (i.e. its name starts with AccountPrefix). Safe to call every login
    // -- does a small synchronous AccountMgr lookup, not a network round
    // trip.
    [[nodiscard]] bool IsAutonomousPlayerAccount(uint32_t accountId);

    // Returns the account id for `username`, creating the account first if
    // it doesn't already exist. `username` must start with AccountPrefix;
    // returns 0 (and logs an error) otherwise -- this keeps
    // IsAutonomousPlayerAccount's ownership check correct by construction.
    uint32_t EnsureBotAccount(std::string const& username, std::string const& password);

    // Constructs a bot-flagged WorldSession (sock == nullptr, IsBot() ==
    // true -- see ADR-008) for the given account and registers it with
    // WorldSessionMgr, exactly like a real client's session would be
    // registered on successful auth. Ownership transfers to
    // WorldSessionMgr; the caller must not delete the returned pointer.
    WorldSession* CreateBotSession(uint32_t accountId, std::string const& accountName);

    // Submits a real character-creation request on `session` via the same
    // public opcode handler (WorldSession::HandleCharCreateOpcode) a game
    // client uses -- so name/race/class validation, starting stats, and
    // starting inventory are produced by the same code as a normal player.
    // This is asynchronous: the character will not exist immediately after
    // this call returns. Poll for completion with
    // sCharacterCache->GetCharacterGuidByName(name).
    void SubmitCharacterCreate(
        WorldSession* session,
        std::string const& name,
        uint8_t race,
        uint8_t characterClass,
        uint8_t gender);
} // namespace AutonomousPlayer::Setup

#endif // AUTONOMOUS_PLAYER_BOT_PROVISIONING_H
