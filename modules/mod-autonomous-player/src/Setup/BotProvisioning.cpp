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

#include "BotProvisioning.h"
#include "AccountMgr.h"
#include "Common.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "SharedDefines.h"
#include "Telemetry/Telemetry.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>

namespace AutonomousPlayer::Setup
{
    bool IsAutonomousPlayerAccount(uint32_t accountId)
    {
        std::string name;
        if (!AccountMgr::GetName(accountId, name))
        {
            return false;
        }

        // AccountMgr::CreateAccount uppercases every stored username
        // (Utf8ToUpperOnlyLatin), so `name` here is always e.g. "AP_TEST1"
        // regardless of what case the operator typed when provisioning --
        // a plain name.starts_with(AccountPrefix) against the lowercase
        // AccountPrefix is always false. Confirmed live on zoidberg: login
        // silently completed core-side (characters.online was set to 1)
        // but this check rejected it, so PLAYERHOOK_ON_LOGIN never
        // registered the bot. Compare case-insensitively instead.
        std::string_view prefix(AccountPrefix);
        if (name.size() < prefix.size())
        {
            return false;
        }

        return std::equal(prefix.begin(), prefix.end(), name.begin(),
            [](char a, char b)
            {
                return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
            });
    }

    uint32_t EnsureBotAccount(std::string const& username, std::string const& password)
    {
        if (!username.starts_with(AccountPrefix))
        {
            LOG_ERROR(Telemetry::LogCategory,
                "EnsureBotAccount: refusing to create '{}' -- bot account names must start with '{}'.",
                username, AccountPrefix);
            return 0;
        }

        if (uint32 existingId = AccountMgr::GetId(username))
        {
            return existingId;
        }

        AccountOpResult result = sAccountMgr->CreateAccount(username, password);
        if (result != AOR_OK)
        {
            LOG_ERROR(Telemetry::LogCategory,
                "EnsureBotAccount: failed to create account '{}' (AccountOpResult {}).",
                username, static_cast<int>(result));
            return 0;
        }

        return AccountMgr::GetId(username);
    }

    WorldSession* CreateBotSession(uint32_t accountId, std::string const& accountName)
    {
        WorldSession* session = new WorldSession(
            accountId,
            std::string(accountName),
            /*accountFlags*/ 0,
            /*sock*/ nullptr,
            /*sec*/ SEC_PLAYER,
            /*expansion*/ EXPANSION_WRATH_OF_THE_LICH_KING,
            /*mute_time*/ 0,
            /*locale*/ DEFAULT_LOCALE,
            /*recruiter*/ 0,
            /*isARecruiter*/ false,
            /*skipQueue*/ true,
            /*TotalTime*/ 0,
            /*is_bot*/ true);

        // Deliberately NOT registered with sWorldSessionMgr -- see the
        // header comment on this function and ADR-008.
        return session;
    }

    std::string ValidateCharacterName(std::string const& name)
    {
        uint8 result = ObjectMgr::CheckPlayerName(name, /*create*/ true);
        switch (result)
        {
            case CHAR_NAME_SUCCESS:
                return {};
            case CHAR_NAME_NO_NAME:
                return "empty name";
            case CHAR_NAME_TOO_SHORT:
                return "too short";
            case CHAR_NAME_TOO_LONG:
                return "too long";
            case CHAR_NAME_INVALID_CHARACTER:
                return "contains an invalid character (note: digits are not allowed in a real character name)";
            case CHAR_NAME_MIXED_LANGUAGES:
                return "mixed languages/character sets";
            case CHAR_NAME_PROFANE:
                return "profane";
            case CHAR_NAME_RESERVED:
                return "reserved";
            case CHAR_NAME_INVALID_APOSTROPHE:
            case CHAR_NAME_MULTIPLE_APOSTROPHES:
                return "invalid apostrophe usage";
            case CHAR_NAME_THREE_CONSECUTIVE:
                return "three consecutive identical letters";
            case CHAR_NAME_INVALID_SPACE:
            case CHAR_NAME_CONSECUTIVE_SPACES:
                return "invalid space usage";
            default:
                return "rejected by ObjectMgr::CheckPlayerName (code " + std::to_string(result) + ")";
        }
    }

    void SubmitCharacterCreate(
        WorldSession* session,
        std::string const& name,
        uint8_t race,
        uint8_t characterClass,
        uint8_t gender)
    {
        WorldPacket packet(CMSG_CHAR_CREATE, 32);
        packet << name;
        packet << uint8(race);
        packet << uint8(characterClass);
        packet << uint8(gender);
        packet << uint8(0); // skin
        packet << uint8(0); // face
        packet << uint8(0); // hair style
        packet << uint8(0); // hair color
        packet << uint8(0); // facial hair
        packet << uint8(0); // outfit id (client-side only, ignored server-side)

        session->HandleCharCreateOpcode(packet);
    }
} // namespace AutonomousPlayer::Setup
