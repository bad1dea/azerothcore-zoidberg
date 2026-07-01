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

#include "BotCombat.h"
#include "MotionMaster.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutonomousPlayer::Combat
{
    bool RequestAttack(Player* bot, ObjectGuid const& targetGuid)
    {
        if (!bot || !bot->GetSession())
        {
            return false;
        }

        WorldPacket packet(CMSG_ATTACKSWING, 8);
        packet << targetGuid;

        bot->GetSession()->HandleAttackSwingOpcode(packet);

        // Unit::Attack() (called by the handler above) only sets combat
        // state -- it does NOT add any follow/chase movement for the
        // attacker. A real human player's client handles staying in melee
        // range via their own WASD/click-move input; our bot has none, so
        // without this it stands still and the fight silently stalls the
        // moment the target moves even slightly (confirmed live: a fled
        // Mottled Boar left the bot stuck at 66/70 hp, combat=true,
        // position frozen, for 20+ seconds). MoveChase is the same
        // production movement generator core NPC AI uses to stay on a
        // target (real navmesh pathing, not a teleport) -- this is the
        // Combat component actually needing a minimal slice of what
        // Navigation already provides, not new pathing logic.
        if (Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid))
        {
            bot->GetMotionMaster()->MoveChase(target);
        }

        return true;
    }

    SpellCastResult RequestCastSpell(Unit* caster, Unit* target, uint32_t spellId)
    {
        if (!caster || !target)
        {
            return SPELL_FAILED_BAD_TARGETS;
        }

        return caster->CastSpell(target, spellId, false);
    }
} // namespace AutonomousPlayer::Combat
