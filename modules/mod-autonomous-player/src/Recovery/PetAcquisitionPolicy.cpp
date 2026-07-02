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

#include "PetAcquisitionPolicy.h"
#include "Creature.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace AutonomousPlayer::Recovery
{
    namespace
    {
        // How far to look for a tameable beast already worth acting on.
        // Deliberately small -- this slice does not move the bot toward
        // one, so a wide radius would just mean "found, but too far to
        // act on" most of the time. 20 yards comfortably covers Tame
        // Beast's own real cast range plus normal guide-movement drift.
        constexpr float PetAcquisitionSearchRadiusYards = 20.0f;
    } // namespace

    std::optional<Combat::CombatIntent> PlanPetAcquisition(Player* bot, Pets::PetState state)
    {
        // ADR-042 follow-up fix (`KNOWN_FAILURES.md` #19): a dead bot
        // can't cast anything -- see `PlanPetRecovery`'s matching check
        // for the full real-bug explanation (a doomed instant-fail cast
        // attempt every tick, combined with `TickAmbient`'s skip-Tick()
        // behavior, permanently starves the bot's own guide-step
        // dispatch with no bounded-wait escape).
        if (!bot || !bot->IsAlive() || bot->IsInCombat() || state != Pets::PetState::NoPet)
        {
            return std::nullopt;
        }

        // Deliberately NOT `bot->HasSpell(Pets::TameBeastSpellId)` --
        // found live to be the wrong check (`KNOWN_FAILURES.md` #15):
        // Tame Beast is a real, innate Hunter ability, not one granted
        // through the normal trainer/spellbook system, so `HasSpell`
        // returns false for it even on a Hunter who can genuinely cast
        // it right now (confirmed live: `.autonomousplayer tamebeast`
        // succeeded with `SPELL_CAST_OK` against a level-1 Hunter whose
        // spellbook does not list spell 1515 at all). An earlier version
        // of this check used `HasSpell` here and would have silently
        // never fired for any Hunter, ever -- caught before being left
        // as a real gap. `IsClass(CLASS_HUNTER, CLASS_CONTEXT_ABILITY)`
        // is a cheap pre-filter only (avoids a wasted grid search for
        // every non-Hunter bot every tick) -- the real, authoritative
        // class/level/range/etc. validation still happens inside the
        // engine's own `CheckCast` when the cast is actually attempted,
        // same "delegate to the real engine" philosophy as every other
        // primitive in this module.
        if (!bot->IsClass(CLASS_HUNTER, CLASS_CONTEXT_ABILITY))
        {
            return std::nullopt;
        }

        // Same ADR-040 fix as `PlanPetRecovery`: Tame Beast has a real
        // cast time too, so re-issuing it every eligible tick would
        // interrupt and restart its own in-flight cast before it could
        // ever complete, exactly like the RecoverPet/CallPet bug this
        // mirrors.
        if (bot->IsNonMeleeSpellCast(false))
        {
            return std::nullopt;
        }

        Creature* beast = Pets::FindNearestTameableBeast(bot, PetAcquisitionSearchRadiusYards);
        if (!beast)
        {
            return std::nullopt;
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(Pets::TameBeastSpellId);
        float maxRange = spellInfo ? spellInfo->GetMaxRange(true, bot) : 0.0f;
        if (maxRange <= 0.0f || bot->GetDistance(beast) > maxRange)
        {
            // Found, but out of the spell's own real range -- slice 1
            // does not chase it (see this function's header comment).
            return std::nullopt;
        }

        return Combat::CombatIntent{ Combat::IntentKind::AcquirePet, beast->GetGUID(), 0 };
    }
} // namespace AutonomousPlayer::Recovery
