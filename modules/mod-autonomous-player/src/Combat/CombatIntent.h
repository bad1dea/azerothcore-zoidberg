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

#ifndef AUTONOMOUS_PLAYER_COMBAT_INTENT_H
#define AUTONOMOUS_PLAYER_COMBAT_INTENT_H

#include "ObjectGuid.h"
#include <cstdint>

namespace AutonomousPlayer::Combat
{
    // What the bot's decision layer (`GuideRuntime`, and later a real
    // Planner) *wants* done, decoupled from which specific opcode/spell
    // primitive does it. Introduced specifically so pet-maintenance
    // logic (and any future combat behavior) goes through one shared,
    // reviewable execution path instead of each new capability adding
    // its own scattered `if` calling a primitive directly from inside
    // `GuideRuntime`'s tick functions -- the pattern this replaces (see
    // ARCHITECTURE.md ADR-039).
    enum class IntentKind : uint8_t
    {
        EngageTarget,      // melee-engage a hostile target (Combat::RequestAttack)
        UseAbility,        // cast a spell at a target (Combat::RequestCastSpell)
        AssistPetOnTarget, // command the pet onto a specific target (Pets::RequestAttackTarget)
        RecoverPet,        // pet is dead (Active or Missing) -- Pets::RequestRevivePet
        CallPet,           // pet is missing but alive -- Pets::RequestCallPet
        ClearStalePetSlot, // Pets::RequestClearStalePetSlot, see KNOWN_FAILURES.md #13
    };

    struct CombatIntent
    {
        IntentKind Kind;
        ObjectGuid Target;    // meaningless for RecoverPet/CallPet/ClearStalePetSlot
        uint32_t SpellId = 0; // only meaningful for UseAbility
    };
} // namespace AutonomousPlayer::Combat

#endif // AUTONOMOUS_PLAYER_COMBAT_INTENT_H
