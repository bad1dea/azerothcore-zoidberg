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

#include "BotSessionMgr.h"
#include "WorldSession.h"

#include <algorithm>

namespace AutonomousPlayer::Lifecycle
{
    BotSessionMgr* BotSessionMgr::Instance()
    {
        static BotSessionMgr instance;
        return &instance;
    }

    void BotSessionMgr::TrackSession(WorldSession* session)
    {
        if (!session)
        {
            return;
        }

        if (std::find(_sessions.begin(), _sessions.end(), session) != _sessions.end())
        {
            return;
        }

        _sessions.push_back(session);
    }

    void BotSessionMgr::UntrackAndDelete(WorldSession* session)
    {
        if (!session)
        {
            return;
        }

        auto it = std::find(_sessions.begin(), _sessions.end(), session);
        if (it == _sessions.end())
        {
            return;
        }

        _sessions.erase(it);
        delete session;
    }

    void BotSessionMgr::QueueForRemoval(WorldSession* session)
    {
        if (!session)
        {
            return;
        }

        auto it = std::find(_sessions.begin(), _sessions.end(), session);
        if (it == _sessions.end())
        {
            return;
        }

        _sessions.erase(it);
        _pendingRemoval.push_back(session);
    }

    std::size_t BotSessionMgr::GetTrackedCount() const
    {
        return _sessions.size();
    }

    void BotSessionMgr::Update(uint32_t diff)
    {
        for (WorldSession* session : _pendingRemoval)
        {
            delete session;
        }
        _pendingRemoval.clear();

        for (WorldSession* session : _sessions)
        {
            MapSessionFilter filter(session);
            session->Update(diff, filter);
        }
    }
} // namespace AutonomousPlayer::Lifecycle
