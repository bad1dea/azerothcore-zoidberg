/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "BotTransport.h"

#include "Map.h"
#include "Player.h"
#include "Transport.h"

namespace AutonomousPlayer::TransportBehaviors
{
    Transport* FindTransport(Player* bot, uint32_t entry)
    {
        if (!bot || !bot->IsInWorld())
            return nullptr;
        for (Transport* transport : bot->GetMap()->GetAllTransports())
            if (transport && transport->GetEntry() == entry)
                return transport;
        return nullptr;
    }

    bool TransportNear(Transport* transport, float x, float y, float z, float radius)
    {
        if (!transport)
            return false;
        return transport->GetDistance(x, y, z) <= radius;
    }

    bool BoardTransport(Player* bot, Transport* transport)
    {
        if (!bot || !transport || bot->GetTransport() == transport)
            return false;

        // Replicate the server-side result of a client stepping onto a moving
        // transport (MovementHandler.cpp): bind the mover to the transport and
        // register it as a passenger so the transport carries it (and, for a
        // MotionTransport, teleports it across the map boundary mid-route). The
        // passenger's on-transport offset is computed from its current world
        // position relative to the transport.
        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = bot->GetPositionZ();
        float o = bot->GetOrientation();
        transport->CalculatePassengerOffset(x, y, z, &o);
        bot->m_movementInfo.transport.pos.Relocate(x, y, z, o);
        bot->m_movementInfo.transport.guid = transport->GetGUID();

        bot->SetTransport(transport);
        transport->AddPassenger(bot);
        return true;
    }

    bool LeaveTransport(Player* bot)
    {
        if (!bot)
            return false;
        Transport* transport = bot->GetTransport();
        if (!transport)
            return false;
        transport->RemovePassenger(bot);
        bot->SetTransport(nullptr);
        bot->m_movementInfo.transport.Reset();
        return true;
    }

    bool IsOnTransport(Player const* bot)
    {
        return bot && bot->GetTransport() != nullptr;
    }
}
