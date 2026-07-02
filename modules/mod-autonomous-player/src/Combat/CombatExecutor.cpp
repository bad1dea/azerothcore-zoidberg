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

#include "CombatExecutor.h"
#include "BotCombat.h"
#include "ObjectAccessor.h"
#include "Pets/BotPets.h"
#include "Player.h"
#include "Unit.h"

namespace AutonomousPlayer::Combat
{
    void Execute(Player* bot, CombatIntent const& intent)
    {
        if (!bot)
        {
            return;
        }

        switch (intent.Kind)
        {
            case IntentKind::EngageTarget:
                RequestAttack(bot, intent.Target);
                break;

            case IntentKind::UseAbility:
            {
                Unit* target = ObjectAccessor::GetUnit(*bot, intent.Target);
                if (target)
                {
                    RequestCastSpell(bot, target, intent.SpellId);
                }
                break;
            }

            case IntentKind::AssistPetOnTarget:
                Pets::RequestAttackTarget(bot, intent.Target);
                break;

            case IntentKind::RecoverPet:
                Pets::RequestRevivePet(bot);
                break;
        }
    }
} // namespace AutonomousPlayer::Combat
