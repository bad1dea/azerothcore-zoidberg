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
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>

namespace AutonomousPlayer::Lifecycle
{
    namespace
    {
        // A real client acknowledges every server-initiated teleport
        // (MSG_MOVE_TELEPORT_ACK same-map, MSG_MOVE_WORLDPORT_ACK across
        // maps), and the core deliberately keeps the player at the old
        // position until that ack arrives (`Player::IsBeingTeleported*`
        // semaphores; `WorldSession::HandleMoveTeleportAck`/
        // `HandleMoveWorldportAck`). A socketless bot session has no
        // client to send them, so any legitimately server-initiated
        // teleport of a bot -- a GM `.tele name`, a summon -- hangs the
        // semaphore forever and silently never moves the bot. Found
        // live (KNOWN_FAILURES.md #22): `.tele name Grunttestbot
        // ValleyOfTrials` printed success and the bot stayed put
        // indefinitely.
        //
        // This synthesizes the ack a real client would send, once per
        // update -- the same synthesized-packet-through-real-handler
        // pattern as every other component in this module. It does NOT
        // initiate teleports (ADR-005 forbids the module teleporting
        // bots as a movement shortcut; both entry points below are
        // no-ops unless the core itself already has a teleport pending
        // for this player).
        void AckPendingTeleport(WorldSession* session)
        {
            Player* player = session->GetPlayer();
            if (!player)
            {
                return;
            }

            if (player->IsBeingTeleportedFar())
            {
                // The core's own server-side entry point for exactly
                // this case ("for server-side calls", WorldSession.h).
                session->HandleMoveWorldportAck();
            }
            else if (player->IsBeingTeleportedNear())
            {
                // No server-side entry point exists for the near case,
                // so feed the real handler the exact packet a real
                // client sends: packed mover guid, then a movement
                // counter and client timestamp the handler reads and
                // ignores.
                WorldPacket ack(MSG_MOVE_TELEPORT_ACK, 8 + 4 + 4);
                ack << player->GetPackGUID();
                ack << uint32(0);
                ack << uint32(0);
                session->HandleMoveTeleportAck(ack);
            }
        }
    }

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

            AckPendingTeleport(session);
        }
    }
} // namespace AutonomousPlayer::Lifecycle
