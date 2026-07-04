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

#include "BotRecovery.h"
#include "Corpse.h"
#include "Opcodes.h"
#include "Player.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Recovery
{
    bool RequestReleaseSpirit(Player* bot)
    {
        if (!bot || !bot->GetSession())
        {
            return false;
        }

        WorldPacket packet(CMSG_REPOP_REQUEST, 1);
        packet << uint8(0); // read_skip<uint8>() on the handler side, value unused

        bot->GetSession()->HandleRepopRequestOpcode(packet);

        return true;
    }

    bool RequestReclaimCorpse(Player* bot)
    {
        if (!bot || !bot->GetSession())
        {
            return false;
        }

        Corpse* corpse = bot->GetCorpse();
        if (!corpse)
        {
            return false;
        }

        WorldPacket packet(CMSG_RECLAIM_CORPSE, 8);
        packet << corpse->GetGUID();

        bot->GetSession()->HandleReclaimCorpseOpcode(packet);

        return true;
    }

    bool RequestSpiritHealerResurrect(Player* bot)
    {
        if (!bot || !bot->GetSession() || bot->IsAlive())
        {
            return false;
        }

        // The ghost spawns at a graveyard and the Spirit Healer
        // (universal creature entry 6491) stands right there. Same
        // real opcode path a player clicking the healer uses
        // (WorldSession::HandleSpiritHealerActivateOpcode ->
        // resurrection with sickness + 25% durability) -- the game's
        // own answer to an unreachable corpse, found necessary live
        // when a bot's corpse sank to the bottom of Stonebull Lake
        // (z -51) where no ghost can walk.
        constexpr uint32 SpiritHealerEntry = 6491;
        Creature* healer = bot->FindNearestCreature(SpiritHealerEntry, 100.0f, true);
        if (!healer)
        {
            return false;
        }

        WorldPacket packet(CMSG_SPIRIT_HEALER_ACTIVATE, 8);
        packet << healer->GetGUID();
        bot->GetSession()->HandleSpiritHealerActivateOpcode(packet);
        return true;
    }
} // namespace AutonomousPlayer::Recovery
