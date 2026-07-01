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

#ifndef AUTONOMOUS_PLAYER_BOT_ECONOMY_H
#define AUTONOMOUS_PLAYER_BOT_ECONOMY_H

#include <cstdint>

class Creature;
class Player;

namespace AutonomousPlayer::Economy
{
    // Buys `count` of `itemId` from `vendor` via the real public opcode
    // handler (WorldSession::HandleBuyItemOpcode, fed a real
    // WorldPackets::Item::BuyItem -- this fork has migrated buy/sell to
    // structured C++ packet classes rather than raw WorldPacket, so no
    // byte-level packet synthesis is needed here, just setting the same
    // fields a real client would). Looks up the item's real vendor slot
    // index directly off the live `Creature::GetVendorItems()` data
    // (a plain public struct) -- there's no need to parse our own no-op
    // outgoing SMSG_LIST_INVENTORY to learn slot numbers.
    //
    // `vendor` must actually sell `itemId` (returns false immediately if
    // not found in its vendor item list) and `bot` must be within real
    // interaction range (checked for real inside the handler via
    // Player::GetNPCIfCanInteractWith, same as every other opcode-reuse
    // function in this module -- not shortcut here).
    //
    // Returns true if the request was submitted (no guarantee of success
    // -- same no-feedback-to-a-socketless-session caveat as everywhere
    // else). Verify via the bot's money/inventory afterward.
    bool BuyItem(Player* bot, Creature* vendor, uint32_t itemId, uint32_t count);

    // Repairs all of `bot`'s equipped/inventory items at `vendor` via the
    // real public opcode handler (WorldSession::HandleRepairItemOpcode,
    // fed a synthesized CMSG_REPAIR_ITEM packet with an empty item GUID,
    // which the handler treats as "repair everything" --
    // Player::DurabilityRepairAll). `vendor` must have the repair NPC
    // flag and be in real interaction range (enforced by the handler).
    bool RepairAll(Player* bot, Creature* vendor);
} // namespace AutonomousPlayer::Economy

#endif // AUTONOMOUS_PLAYER_BOT_ECONOMY_H
