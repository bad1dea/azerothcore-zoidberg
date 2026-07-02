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

#ifndef AUTONOMOUS_PLAYER_COMBAT_EXECUTOR_H
#define AUTONOMOUS_PLAYER_COMBAT_EXECUTOR_H

#include "CombatIntent.h"

class Player;

namespace AutonomousPlayer::Combat
{
    // Turns a `CombatIntent` into the real, already-proven primitive call
    // it represents (`RequestAttack`, `RequestCastSpell`,
    // `Pets::RequestAttackTarget`, `Pets::RequestRevivePet`). This is the
    // one place that maps "what the bot wants" to "which opcode/spell
    // function actually does it" -- callers (currently `GuideRuntime`,
    // later a real Planner) describe intent, they don't call the
    // low-level primitives themselves. No-op on a null `bot` or an
    // intent whose preconditions the underlying primitive itself already
    // rejects for a real reason (e.g. `RecoverPet` with no pet at all) --
    // same "a rejected request for a legitimate reason is not an error"
    // philosophy as every primitive it wraps.
    void Execute(Player* bot, CombatIntent const& intent);
} // namespace AutonomousPlayer::Combat

#endif // AUTONOMOUS_PLAYER_COMBAT_EXECUTOR_H
