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

#include "SharedDefines.h"
#include <cstdint>

class ObjectGuid;
class Player;
class Unit;

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

    // Casts `spellId` at `target` via the real public core API
    // `Unit::CastSpell(target, spellId, /*triggered=*/false)` -- runs the
    // full real spell pipeline (cost, cooldown, range, line-of-sight, GCD)
    // exactly as a genuine client-driven cast would. Explicitly allowed
    // by ADR-005 ("the real spell-cast path -- Unit::CastSpell and
    // friends").
    //
    // Deliberately NOT synthesized as CMSG_CAST_SPELL like every other
    // component in this module: that packet's target-data payload varies
    // per spell's implicit target mask (self/unit/location/item/no
    // target, in various combinations), so hand-reconstructing it
    // correctly for an arbitrary spell is real, non-trivial engine work
    // with no corresponding payoff -- the public API this project already
    // allow-lists gets the same real validation with far less risk of a
    // subtly-wrong synthesized packet silently mis-casting. See ADR-018.
    //
    // Returns the real `SpellCastResult` from `Unit::CastSpell` --
    // `SPELL_CAST_OK` on success, or the actual reason otherwise (e.g.
    // `SPELL_FAILED_NO_POWER`, `SPELL_FAILED_BAD_TARGETS`,
    // `SPELL_FAILED_NOT_READY` for a cooldown, `SPELL_FAILED_ONLY_ABOVEWATER`-
    // style state-gated rejections, etc.) -- unlike the
    // socketless-session opcode functions elsewhere in this module, this
    // one gives a real, specific result code from the same synchronous
    // call, not just "submitted." Deliberately not collapsed to a bool:
    // finding out *why* a cast was rejected is the whole point (see
    // KNOWN_FAILURES.md #4 for a real case where a bool alone wasn't
    // enough to diagnose a rejected cast).
    SpellCastResult RequestCastSpell(Unit* caster, Unit* target, uint32_t spellId);
} // namespace AutonomousPlayer::Combat

#endif // AUTONOMOUS_PLAYER_BOT_COMBAT_H
