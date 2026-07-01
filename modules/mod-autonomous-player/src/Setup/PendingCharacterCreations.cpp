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
                // Deliberately do NOT UntrackAndDelete here. Confirmed
                // live on zoidberg: hitting this timeout does not mean
                // the async CharacterDatabase/LoginDatabase chain is
                // dead -- it can still complete well after this fires
                // (observed completion ~30-60s after the timeout log).
                // Deleting the session while that chain might still
                // reference it (its completion callbacks capture `this`)
                // is a use-after-free -- it happened to not crash the one
                // time this was tried, but that's luck, not correctness.
                // We just stop actively polling; the session stays
                // tracked (and harmlessly ticked forever) in
                // BotSessionMgr so any in-flight work can still complete
                // safely. A stuck session here is a resource leak to
                // investigate, not something to guess-and-delete.
                LOG_ERROR(Telemetry::LogCategory,
                    "PendingCharacterCreations: '{}' did not appear after {} ticks -- giving up polling, "
                    "but NOT deleting the session (async work may still be in flight). Check "
                    "acore_characters.characters for account {} manually; if a row eventually appears, "
                    "this timeout is too short and should be raised, not worked around by deleting sooner.",
                    entry.CharacterName, entry.TicksWaited, entry.Session->GetAccountId());
                it = Entries.erase(it);
                continue;
            }

            ++it;
        }
    }
} // namespace AutonomousPlayer::Setup::PendingCharacterCreations
