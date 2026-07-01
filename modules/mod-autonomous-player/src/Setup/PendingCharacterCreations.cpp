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

#include "PendingCharacterCreations.h"
#include "CharacterCache.h"
#include "Lifecycle/BotSessionMgr.h"
#include "Log.h"
#include "Telemetry/Telemetry.h"
#include "WorldSession.h"

#include <vector>

namespace AutonomousPlayer::Setup::PendingCharacterCreations
{
    namespace
    {
        struct Entry
        {
            WorldSession* Session;
            std::string CharacterName;
            uint32_t TicksWaited = 0;
        };

        std::vector<Entry> Entries;
    }

    void Watch(WorldSession* session, std::string const& characterName)
    {
        if (!session)
        {
            return;
        }

        Entries.push_back(Entry{ session, characterName, 0 });
    }

    void Update()
    {
        for (auto it = Entries.begin(); it != Entries.end();)
        {
            Entry& entry = *it;

            if (sCharacterCache->GetCharacterGuidByName(entry.CharacterName))
            {
                LOG_INFO(Telemetry::LogCategory,
                    "PendingCharacterCreations: '{}' created successfully.", entry.CharacterName);
                sBotSessionMgr->UntrackAndDelete(entry.Session);
                it = Entries.erase(it);
                continue;
            }

            if (++entry.TicksWaited > TimeoutTicks)
            {
                LOG_ERROR(Telemetry::LogCategory,
                    "PendingCharacterCreations: '{}' did not appear after {} ticks -- creation failed "
                    "or is still pending; check for CHAR_CREATE_* validation issues (name in use, "
                    "disabled race/class, etc).",
                    entry.CharacterName, entry.TicksWaited);
                sBotSessionMgr->UntrackAndDelete(entry.Session);
                it = Entries.erase(it);
                continue;
            }

            ++it;
        }
    }
} // namespace AutonomousPlayer::Setup::PendingCharacterCreations
