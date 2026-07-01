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

#include "BotLifecycleMgr.h"

namespace AutonomousPlayer
{
    BotLifecycleMgr* BotLifecycleMgr::Instance()
    {
        static BotLifecycleMgr instance;
        return &instance;
    }

    void BotLifecycleMgr::RegisterBot(ObjectGuid guid)
    {
        _sessions.try_emplace(guid, BotSession{ guid, 0, 0 });
    }

    void BotLifecycleMgr::UnregisterBot(ObjectGuid guid)
    {
        _sessions.erase(guid);
    }

    bool BotLifecycleMgr::IsRegistered(ObjectGuid guid) const
    {
        return _sessions.contains(guid);
    }

    std::size_t BotLifecycleMgr::GetBotCount() const
    {
        return _sessions.size();
    }

    void BotLifecycleMgr::Update(uint32_t diff)
    {
        for (auto& [guid, session] : _sessions)
        {
            session.AccumulatedMs += diff;

            while (session.AccumulatedMs >= TickIntervalMs)
            {
                session.AccumulatedMs -= TickIntervalMs;
                ++session.TickCount;
            }
        }
    }

    uint32_t BotLifecycleMgr::GetTickCount(ObjectGuid guid) const
    {
        auto it = _sessions.find(guid);
        return it != _sessions.end() ? it->second.TickCount : 0;
    }
} // namespace AutonomousPlayer
