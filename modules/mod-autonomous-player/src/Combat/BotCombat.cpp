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
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>

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

    bool RequestAttackRanged(Player* bot, ObjectGuid const& targetGuid, uint32_t openerSpellId)
    {
        if (!bot)
        {
            return false;
        }

        Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
        if (!target)
        {
            return false;
        }

        SpellInfo const* opener = sSpellMgr->GetSpellInfo(openerSpellId);
        if (!opener)
        {
            return false;
        }

        // Same combat relationship the melee path establishes, minus
        // melee auto-swings -- `HandleAttackSwingOpcode` itself is a
        // thin wrapper around this call with `meleeAttack=true`.
        // Harmless no-op when already attacking this target.
        bot->Attack(target, false);

        // Hold inside the opener's real range rather than closing to
        // melee contact -- the engine's own chase generator stops
        // there. Clamped so a barely-eligible opener can't produce a
        // degenerate hold distance.
        float holdDistance = std::max(opener->GetMaxRange(false) - RangedHoldBufferYards, 5.0f);
        bot->GetMotionMaster()->MoveChase(target, holdDistance);

        // Open. The in-flight-cast guard is the ADR-040 lesson (per-tick
        // re-issue must not self-interrupt a cast-time opener); an
        // already-autorepeating Auto Shot also trips it, correctly --
        // the engine's own ranged-attack timer keeps firing regardless
        // (see `Unit::_UpdateAutoRepeatSpell`). An out-of-range
        // rejection while still approaching is a harmless, expected
        // no-op -- this function is re-issued every Approaching tick,
        // same as the melee path.
        if (!bot->IsNonMeleeSpellCast(false))
        {
            bot->CastSpell(target, openerSpellId, false);
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

    void MaintainFacing(Player* bot)
    {
        if (!bot)
        {
            return;
        }

        Unit* victim = bot->GetVictim();
        if (!victim || !victim->IsAlive())
        {
            return;
        }

        if (bot->HasInArc(2 * M_PI / 3, victim))
        {
            return;
        }

        // Don't fight an active travel spline (the moonwalk fix) --
        // BUT the guide re-issues MoveChase every combat tick, which
        // keeps the spline perpetually un-finalized, and the first
        // version of this guard therefore suppressed re-facing for
        // the WHOLE fight: the #24 facing bug returned through the
        // side door and the overnight fleet bled (+40 deaths/window,
        // hunters untouched, every melee bot dying -- the class split
        // was the tell). At melee contact the bot is not genuinely
        // traveling no matter what the spline object says: re-face.
        bool const traveling = (bot->isMoving() || !bot->movespline->Finalized())
            && !bot->IsWithinMeleeRange(victim);
        if (traveling)
        {
            return;
        }

        bot->SetFacingToObject(victim);
    }
} // namespace AutonomousPlayer::Combat
