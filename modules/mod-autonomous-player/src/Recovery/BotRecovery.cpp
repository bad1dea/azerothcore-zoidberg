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
#include "Item.h"
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

    bool RequestUseHearthstone(Player* bot)
    {
        if (!bot || !bot->GetSession() || !bot->IsAlive())
        {
            return false;
        }

        // The hearthstone (item 6948, spell 8690) is every real
        // player's cross-continent recovery -- found necessary live
        // when fleet bots wandered onto transports (the Brill zeppelin
        // tower) and woke up on the wrong continent, where no amount
        // of walking brings them home. Real cast: 10s channel, 60min
        // cooldown, engine-validated via the same use-item opcode a
        // client sends.
        constexpr uint32 HearthstoneItemId = 6948;
        constexpr uint32 HearthstoneSpellId = 8690;
        Item* stone = bot->GetItemByEntry(HearthstoneItemId);
        if (!stone || stone->IsEquipped())
        {
            return false;
        }

        WorldPacket packet(CMSG_USE_ITEM, 1 + 1 + 1 + 4 + 8 + 4 + 1 + 4);
        packet << uint8(stone->GetBagSlot());
        packet << uint8(stone->GetSlot());
        packet << uint8(0);                     // cast count
        packet << uint32(HearthstoneSpellId);
        packet << stone->GetGUID();
        packet << uint32(0);                    // glyph index
        packet << uint8(0);                     // cast flags
        packet << uint32(0);                    // target flags: TARGET_FLAG_NONE
        bot->GetSession()->HandleUseItemOpcode(packet);
        return true;
    }
} // namespace AutonomousPlayer::Recovery
