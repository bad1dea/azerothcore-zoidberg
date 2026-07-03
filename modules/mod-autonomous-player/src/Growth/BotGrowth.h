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

#ifndef AUTONOMOUS_PLAYER_BOT_GROWTH_H
#define AUTONOMOUS_PLAYER_BOT_GROWTH_H

#include <cstdint>
#include <optional>

class Creature;
class Player;

namespace AutonomousPlayer::Growth
{
    // Opens `trainer`'s spell list via the real public opcode handler
    // (WorldSession::HandleTrainerListOpcode, fed a real
    // WorldPackets::NPC::Hello struct) -- matches the real client flow of
    // selecting the GOSSIP_OPTION_TRAINER option (see Gossip component)
    // and having the trainer window open. `bot` must be within real
    // interaction range and `trainer` must have UNIT_NPC_FLAG_TRAINER
    // (enforced for real inside the handler, not shortcut here).
    bool RequestTrainerList(Player* bot, Creature* trainer);

    // Finds the first spell in `trainer`'s real spell list
    // (sObjectMgr->GetTrainer(trainer->GetEntry())->GetSpells()) that
    // `bot` can actually learn right now -- real level/skill/money
    // requirements via Trainer::CanTeachSpell, not re-derived. Reads the
    // live trainer data directly rather than parsing our own no-op
    // outgoing SMSG_TRAINER_LIST, same pattern as loot/vendor/gossip.
    std::optional<uint32_t> FindLearnableTrainerSpell(Player* bot, Creature* trainer);

    // Requests to learn `spellId` from `trainer` via the real public
    // opcode handler (WorldSession::HandleTrainerBuySpellOpcode, fed a
    // real WorldPackets::NPC::TrainerBuySpell struct) -- runs the real
    // Trainer::TeachSpell (money/skill checks, spell learning) with no
    // shortcuts.
    bool RequestLearnSpell(Player* bot, Creature* trainer, uint32_t spellId);

    // Equips every carried weapon/armor item that fills an EMPTY
    // equipment slot or beats the equipped item's ItemLevel, via the
    // real client equip path (CMSG_AUTOEQUIP_ITEM ->
    // WorldSession::HandleAutoEquipItemOpcode) -- the engine's own
    // CanEquipItem does the real class/level/proficiency validation;
    // this function only chooses candidates. Success is counted from
    // real post-equip item state (Item::IsEquipped), never assumed
    // from having sent the packet. Returns how many items genuinely
    // ended up equipped. Closes the Gate 3 "growth chores" gap where
    // quest rewards accumulated in bags while the bot kept fighting
    // in its level-1 starting kit.
    uint32_t EquipBagUpgrades(Player* bot);
} // namespace AutonomousPlayer::Growth

#endif // AUTONOMOUS_PLAYER_BOT_GROWTH_H
