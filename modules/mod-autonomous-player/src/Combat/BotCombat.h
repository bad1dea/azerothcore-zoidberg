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

    // Ranged-engage (ADR-044): establishes the same real combat
    // relationship as `RequestAttack` but holds at the opener spell's
    // real range instead of closing to melee -- the "ranged pulls as a
    // distinct behavior" Gate 3 bar. Concretely:
    //
    // - `Unit::Attack(target, /*meleeAttack=*/false)` -- the same public
    //   engine call the melee opcode handler makes internally, minus
    //   melee auto-swings; sets victim/combat state so `GuideRuntime`'s
    //   `GetVictim()`-based engagement confirmation works unchanged.
    // - `MotionMaster::MoveChase(target, holdDistance)` where
    //   `holdDistance` is derived from the opener's real `SpellInfo`
    //   max range (minus `RangedHoldBufferYards`) -- the engine's own
    //   chase generator stops at that range instead of melee contact
    //   ("stop movement at the correct range," per the Singular
    //   research doc's approach model).
    // - Casts `openerSpellId` at the target (full real validation).
    //   Guarded by `Unit::IsNonMeleeSpellCast` so a cast-time opener is
    //   not self-interrupted by per-tick re-issue (the ADR-040 lesson);
    //   an already-running Auto Shot (75) makes that check true, which
    //   is exactly right -- the engine keeps autorepeating on its own
    //   ranged-attack timer and re-casting is unnecessary (confirmed by
    //   reading `Unit::_UpdateAutoRepeatSpell`: Auto Shot is never
    //   interrupted by a failed per-shot range check, movement, or
    //   re-cast, and its shot timer is independent of cast requests).
    //
    // Does NOT decide when melee fallback is appropriate (the target
    // closing to melee anyway) -- that's the caller's policy
    // (`TickKillNearest`'s Engaged phase switches to `EngageTarget`
    // when the target is within melee reach). Returns false if the
    // target can't be resolved or the opener has no real SpellInfo.
    bool RequestAttackRanged(Player* bot, ObjectGuid const& targetGuid, uint32_t openerSpellId);

    // The margin held inside the opener's real max range when ranged-
    // engaging (a target at exactly max range drifts out of range with
    // any movement; the engine's own per-shot CheckCast then just skips
    // shots until back in range).
    inline constexpr float RangedHoldBufferYards = 5.0f;

    // The opener's real SpellInfo max range must be at least this for a
    // ranged engage to make sense -- below it (melee-ish abilities,
    // e.g. Warrior Heroic Strike's 5yd) the ordinary melee engage is
    // correct. Auto Shot's real max range is 35yd in this fork's DBC.
    inline constexpr float RangedPullMinimumMaxRangeYards = 15.0f;

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

    // Cast the best available ability from the bot's per-class priority
    // rotation this tick (DoTs kept up, self-buffs kept up, then burst,
    // then filler nuke), picking the highest known rank and letting the
    // engine's real CheckCast (cooldown/power/range/combo/seal) decide
    // castability. Returns true if something was cast. This replaces the
    // old single-"opportunistic-spell" model so a bot fights like a
    // leveling player (rotation) instead of only auto-attacking. Melee
    // auto-attack continues underneath regardless; call once per Engaged
    // tick. Returns false when nothing is castable (pure auto-attack).
    bool CastRotationAbility(Player* bot, Unit* target);

    // Keep the bot facing its current victim (KNOWN_FAILURES.md #24's
    // real root cause, finally): a real client streams orientation
    // updates continuously and auto-faces on attack -- a socketless
    // bot has nobody doing that, so when a target strafes behind it
    // inside melee range (no chase spline gets generated for a
    // within-range target), every melee swing and frontal-arc cast
    // silently skips FOREVER. Live proof: a fight where the bot dealt
    // literally zero damage for 90+ seconds to a full-health green mob
    // while `castspell` returned SPELL_FAILED_UNIT_NOT_INFRONT (134)
    // and rage stayed at 4 (no swings = no rage). Re-faces only when
    // genuinely out of arc and not mid-move (a moving chase orients
    // itself); cheap enough to call every combat tick.
    void MaintainFacing(Player* bot);
} // namespace AutonomousPlayer::Combat

#endif // AUTONOMOUS_PLAYER_BOT_COMBAT_H
