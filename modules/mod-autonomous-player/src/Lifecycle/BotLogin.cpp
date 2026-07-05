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
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Setup/BotProvisioning.h"
#include "Telemetry/Telemetry.h"
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

        // The character must actually belong to the requested account.
        // Without this check a mismatched pair reaches Player::LoadFromDB,
        // whose "loading from wrong account" failure path kicks a session
        // that has no socket -- live on zoidberg (2026-07-05) that took the
        // whole worldserver down. Refuse up front instead.
        CharacterCacheEntry const* cacheEntry = sCharacterCache->GetCharacterCacheByGuid(characterGuid);
        if (!cacheEntry || cacheEntry->AccountId != accountId)
        {
            LOG_ERROR(Telemetry::LogCategory,
                "TryLoginBot: character '{}' belongs to account {}, not '{}' ({}) -- refusing login.",
                characterName, cacheEntry ? cacheEntry->AccountId : 0, accountName, accountId);
            return false;
        }

        // Deliberately does NOT go through WorldSession::HandlePlayerLoginOpcode
        // (even though it's public): that function first checks
        // IsLegitCharacterForAccount(guid), which only ever returns true for
        // GUIDs already present in _legitCharacters -- a set populated by the
        // character-list-*enumeration* flow (HandleCharEnum/BuildEnumData)
        // that a real client runs before ever sending CMSG_PLAYER_LOGIN. Our
        // bot never enumerates a character list, so that set is always
        // empty and login would always be rejected with "Account can't
        // login with that character." Confirmed live on zoidberg.
        //
        // mod-playerbots' own bot sessions (constructed the same
        // sock=nullptr way -- see ADR-008) sidestep this the same way we
        // do here: call the also-public WorldSession::HandlePlayerLoginFromDB
        // directly, driven by our own LoginQueryHolder, instead of going
        // through the opcode entry point built for network clients. This is
        // the same real, production login-finalization code
        // (HandlePlayerLoginOpcode calls this exact function once its own
        // holder resolves) -- we're just skipping the client-only
        // gatekeeping step ahead of it, not reimplementing login.
        auto holder = std::make_shared<LoginQueryHolder>(accountId, characterGuid);
        if (!holder->Initialize())
        {
            LOG_ERROR(Telemetry::LogCategory,
                "TryLoginBot: LoginQueryHolder::Initialize failed for '{}'.", characterName);
            return false;
        }

        WorldSession* session = Setup::CreateBotSession(accountId, accountName);

        // Must be tracked before queuing the holder callback below (see
        // ADR-008 -- an untracked session's async work would be orphaned),
        // and stays tracked for the bot's entire online lifetime.
        // Untracked (deferred-deleted) in AutonomousPlayerModule.cpp's
        // OnPlayerLogout hook via QueueForRemoval.
        sBotSessionMgr->TrackSession(session);

        session->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
            [session](SQLQueryHolderBase const& completedHolder)
            {
                session->HandlePlayerLoginFromDB(static_cast<LoginQueryHolder const&>(completedHolder));
            });

        return true;
    }
} // namespace AutonomousPlayer::Lifecycle
