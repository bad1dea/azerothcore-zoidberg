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

#ifndef AUTONOMOUS_PLAYER_BOT_COMBAT_H
#define AUTONOMOUS_PLAYER_BOT_COMBAT_H

class ObjectGuid;
class Player;

namespace AutonomousPlayer::Combat
{
    // Submits a real melee-attack-start request on `bot` via the same
    // public opcode handler (WorldSession::HandleAttackSwingOpcode) a
    // game client uses when the player clicks/keys an attack on a
    // selected target. `Unit::IsValidAttackTarget` runs for real (hostile,
    // alive, attackable, not a friendly/vehicle-seat edge case) exactly as
    // it would for a human player -- this function does not decide
    // whether the target is *legal*, only that we're asking to attack it.
    //
    // Once started, melee auto-attack swings happen automatically via
    // core's normal per-tick Unit/Map update -- the same mechanism that
    // drives combat for every other Player/Creature in the world. This
    // function also issues a MotionMaster::MoveChase on the target:
    // Unit::Attack() alone only sets combat state, it does NOT keep the
    // attacker in melee range if the target moves (a real client relies
    // on the human player's own movement input for that) -- confirmed
    // live, a fled target left the bot stuck in combat with a frozen
    // position and no further damage either way. This function only
    // initiates the engagement; it does not decide when to stop (see
    // Player::AttackStop for that, wired to a later slice once retreat/
    // kill-detection logic exists).
    //
    // This is deliberately the smallest legitimate slice of the `Combat`
    // component described in the project's Combat requirements: no target
    // selection, no pack-density/approach-safety logic, no ranged/pull/
    // kite/CC/adds handling, no post-combat loot -- those are separate,
    // later slices. Target legality and adjacency here are the caller's
    // responsibility (see the .autonomousplayer attack debug command,
    // which finds and walks to a target before calling this).
    //
    // Returns true if the request was submitted (no guarantee of success
    // -- same no-feedback-to-a-socketless-session caveat as every other
    // opcode-reuse function in this module). Verify with
    // bot->IsInCombat() / the target's health afterward.
    bool RequestAttack(Player* bot, ObjectGuid const& targetGuid);
} // namespace AutonomousPlayer::Combat

#endif // AUTONOMOUS_PLAYER_BOT_COMBAT_H
