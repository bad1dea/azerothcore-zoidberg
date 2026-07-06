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
#include "Log.h"
#include "MotionMaster.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Telemetry/Telemetry.h"
#include "Unit.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <vector>

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

    namespace
    {
        // One rotation entry: a base-rank spell id, whether it's a DoT
        // (don't re-apply while ticking) and whether it targets self
        // (buffs/seals -- don't re-cast while the aura is up).
        struct RotationEntry
        {
            uint32 Base;
            bool IsDoT;
            bool OnSelf;
        };

        // Per-class priority rotation (highest priority first). Base
        // rank-1 ids; the highest rank the bot actually knows is resolved
        // at cast time, so the same table works at every level. Entries
        // the class hasn't learned yet, that are on cooldown, or that
        // lack resource/range are skipped by the real CheckCast -- this
        // is a priority list, not a fixed sequence. DoTs first (apply
        // once, big value over a fight), then instant/burst, then the
        // filler nuke; self-buffs kept up. Not a theorycrafted APL --
        // the "play it like a real leveling player instead of only
        // auto-attacking" baseline.
        std::vector<RotationEntry> const& RotationFor(uint8 cls)
        {
            static std::vector<RotationEntry> const none;
            static std::vector<RotationEntry> const warrior = {
                // Battle Stance first: warrior stances are shapeshift
                // forms, and provisioned bots spawn stance-LESS, so every
                // stance-requiring ability failed ONLY_SHAPESHIFT (94)
                // forever -- cast forensics caught Rend doing exactly
                // that fleet-wide (2026-07-06). OnSelf + HasAura keeps
                // this a one-time cast per life.
                {2457, false, true},   // Battle Stance (required form)
                {6673, false, true},   // Battle Shout (keep up)
                {772,  true,  false},  // Rend (DoT)
                {78,   false, false}}; // Heroic Strike (rage dump)
            static std::vector<RotationEntry> const paladin = {
                {19740, false, true},  // Blessing of Might (keep up)
                {21084, false, true},  // Seal of Righteousness (keep up)
                {20271, false, false}, // Judgement
                {35395, false, false}};// Crusader Strike (higher level; no-op if unknown)
            static std::vector<RotationEntry> const hunter = {
                {13165, false, true},  // Aspect of the Hawk (keep up)
                {1978, true,  false},  // Serpent Sting (DoT, needs ranged+ammo)
                {3044, false, false},  // Arcane Shot
                {2973, false, false}}; // Raptor Strike (melee -- works w/o ammo)
            static std::vector<RotationEntry> const rogue = {
                {2098, false, false},  // Eviscerate (finisher; fails w/o combo -> falls through)
                {1752, false, false}}; // Sinister Strike (builder)
            static std::vector<RotationEntry> const priest = {
                {17,  false, true},    // Power Word: Shield (absorb; CheckCast blocks Weakened Soul)
                {589, true,  false},   // Shadow Word: Pain (DoT)
                {585, false, false}};  // Smite
            static std::vector<RotationEntry> const shaman = {
                {324,  false, true},   // Lightning Shield (keep up)
                {8050, true,  false},  // Flame Shock (DoT)
                {8042, false, false},  // Earth Shock
                {403,  false, false}}; // Lightning Bolt
            static std::vector<RotationEntry> const mage = {
                {168,  false, true},   // Frost Armor (armor + attacker slow -- survival)
                {116,  false, false},  // Frostbolt (slows -- helps survival)
                {2136, false, false},  // Fire Blast (instant)
                // Fireball last: the STARTING nuke. Its absence meant an
                // untrained mage had nothing castable in this table and
                // staff-meleed for 4-5 a swing (forensics, 2026-07-06);
                // for trained mages Frostbolt/Fire Blast win first.
                {133,  false, false}}; // Fireball
            static std::vector<RotationEntry> const warlock = {
                {687, false, true},    // Demon Skin/Armor (keep up)
                {172, true,  false},   // Corruption (DoT)
                {348, true,  false},   // Immolate (DoT)
                {686, false, false}};  // Shadow Bolt
            static std::vector<RotationEntry> const druid = {
                {1126, false, true},   // Mark of the Wild (keep up)
                {8921, true,  false},  // Moonfire (DoT)
                {5176, false, false}}; // Wrath
            switch (cls)
            {
                case CLASS_WARRIOR: return warrior;
                case CLASS_PALADIN: return paladin;
                case CLASS_HUNTER:  return hunter;
                case CLASS_ROGUE:   return rogue;
                case CLASS_PRIEST:  return priest;
                case CLASS_SHAMAN:  return shaman;
                case CLASS_MAGE:    return mage;
                case CLASS_WARLOCK: return warlock;
                case CLASS_DRUID:   return druid;
                default:            return none;
            }
        }

        // Highest rank of a spell chain the bot actually knows (0 if none).
        uint32 BestKnownRank(Player* bot, uint32 baseId)
        {
            uint32 best = 0;
            for (uint32 id = sSpellMgr->GetFirstSpellInChain(baseId); id;
                 id = sSpellMgr->GetNextSpellInChain(id))
            {
                if (bot->HasSpell(id))
                {
                    best = id;
                }
            }
            return best;
        }
    } // namespace

    bool CastRotationAbility(Player* bot, Unit* target)
    {
        if (!bot || !target || !bot->IsAlive() || !target->IsAlive())
        {
            return false;
        }
        // Never stack a new cast on an in-flight one. skipAutorepeat=true
        // is LOAD-BEARING: an armed wand/Auto Shot autorepeat counts as a
        // "non-melee spell cast" otherwise, so the first wand shot locked
        // casters out of their rotation for the rest of the fight --
        // forensics showed level-7 mages dealing 4-5 per action (pure
        // wand) with zero rotation-rejection lines, i.e. this early-out
        // fired before any spell was ever attempted (2026-07-06).
        if (bot->IsNonMeleeSpellCast(false, false, true))
        {
            return false;
        }

        for (RotationEntry const& e : RotationFor(bot->getClass()))
        {
            uint32 const id = BestKnownRank(bot, e.Base);
            if (!id)
            {
                continue;   // not learned at this level
            }
            Unit* dest = e.OnSelf ? static_cast<Unit*>(bot) : target;
            if ((e.IsDoT || e.OnSelf) && dest->HasAura(id, bot->GetGUID()))
            {
                continue;   // DoT still ticking / self-buff still up
            }
            // The engine's own CheckCast (cooldown, power, range, LoS,
            // combo points, seal requirement, ...) decides castability;
            // cast the first that passes -- that's the rotation pick.
            SpellCastResult const result = RequestCastSpell(bot, dest, id);
            if (result == SPELL_CAST_OK)
            {
                return true;
            }
            // Cast forensics (2026-07-06): a trained mage wanded a bear
            // to mutual death without one spell landing and nothing
            // said why. Cooldown churn (NOT_READY) is expected between
            // GCDs and stays quiet; every other rejection is a real
            // diagnosis line.
            if (result != SPELL_FAILED_NOT_READY)
            {
                LOG_INFO(Telemetry::LogCategory,
                    "rotation: '{}' spell {} rejected ({})",
                    bot->GetName(), id, static_cast<uint32>(result));
            }
        }
        return false;
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
