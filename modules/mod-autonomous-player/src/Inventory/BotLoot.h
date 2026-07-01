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

#ifndef AUTONOMOUS_PLAYER_BOT_LOOT_H
#define AUTONOMOUS_PLAYER_BOT_LOOT_H

class Creature;
class Player;

namespace AutonomousPlayer::Inventory
{
    // Loots a dead creature's corpse: opens the loot (real
    // WorldSession::HandleLootOpcode -> Player::SendLoot, same as a real
    // client's right-click), auto-stores every item slot (real
    // WorldSession::HandleAutostoreLootItemOpcode per slot), takes any
    // money (real WorldSession::HandleLootMoneyOpcode), then releases the
    // loot (real WorldSession::HandleLootReleaseOpcode) -- all via
    // synthesized packets through the real opcode handlers, same pattern
    // as every other component in this module.
    //
    // Unlike the SMSG_LOOT_RESPONSE the client would normally parse to
    // learn what's lootable (a no-op send for our socketless session,
    // same caveat as everywhere else), this function reads the slot count
    // directly off the live `corpse->loot.items`/`loot.gold` -- we have
    // direct C++ access to that object, so there's no need to round-trip
    // through our own outgoing packet to know what to loot.
    //
    // `corpse` must be dead and within interaction/loot range of `bot`
    // (same real distance check `HandleAutostoreLootItemOpcode` performs
    // -- this function doesn't move the bot into range itself, see
    // Navigation::MoveTo for that).
    //
    // Returns true if a loot session was opened and released (regardless
    // of whether it actually contained anything -- an empty/junk-only
    // corpse is a legitimate outcome, not a failure). Verify actual
    // results via the bot's inventory/money afterward.
    bool LootCorpse(Player* bot, Creature* corpse);
} // namespace AutonomousPlayer::Inventory

#endif // AUTONOMOUS_PLAYER_BOT_LOOT_H
