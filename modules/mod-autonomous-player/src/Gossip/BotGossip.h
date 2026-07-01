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

#ifndef AUTONOMOUS_PLAYER_BOT_GOSSIP_H
#define AUTONOMOUS_PLAYER_BOT_GOSSIP_H

#include <cstdint>
#include <optional>

class Creature;
class Player;

namespace AutonomousPlayer::Gossip
{
    // Opens a gossip dialogue with `npc` via the real public opcode
    // handler (WorldSession::HandleGossipHelloOpcode, fed a synthesized
    // CMSG_GOSSIP_HELLO packet) -- same as right-clicking an NPC with a
    // gossip menu. This is what actually builds the server-side
    // `Player::PlayerTalkClass->GetGossipMenu()` state (real script/core
    // logic decides what options are offered); the SMSG_GOSSIP_MESSAGE
    // reply is a no-op send for our socketless bot, same caveat as
    // everywhere else, but the menu is inspectable directly afterward via
    // `bot->PlayerTalkClass->GetGossipMenu()` -- no need to parse our own
    // outgoing packet to know what's available.
    bool RequestGossipHello(Player* bot, Creature* npc);

    // Looks for a gossip menu item (in the menu built by a prior
    // RequestGossipHello) whose OptionType matches `optionType` (see
    // GOSSIP_OPTION_* in GossipDef.h, e.g. GOSSIP_OPTION_TRAINER). Returns
    // its list-index if found.
    std::optional<uint32_t> FindGossipOptionIndex(Player* bot, uint32_t optionType);

    // Selects gossip menu item `gossipListId` via the real public opcode
    // handler (WorldSession::HandleGossipSelectOptionOpcode, fed a
    // synthesized CMSG_GOSSIP_SELECT_OPTION packet) -- same as clicking
    // an option in the dialogue. `npc` must be the same NPC the menu was
    // opened with (the handler checks the menu's sender GUID matches).
    // Real, scripted/core-driven consequences run for real (e.g.
    // selecting a GOSSIP_OPTION_TRAINER item opens the real trainer
    // session via Creature::OnGossipSelect) -- this function does not
    // duplicate any of that logic, it only asks for the option.
    bool RequestGossipSelectOption(Player* bot, Creature* npc, uint32_t gossipListId);
} // namespace AutonomousPlayer::Gossip

#endif // AUTONOMOUS_PLAYER_BOT_GOSSIP_H
