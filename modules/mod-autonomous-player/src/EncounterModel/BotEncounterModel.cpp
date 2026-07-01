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

#include "BotEncounterModel.h"
#include "Player.h"
#include "Unit.h"

namespace AutonomousPlayer::EncounterModel
{
    bool Snapshot::HasUnplannedAdd() const
    {
        for (Attacker const& attacker : Attackers)
        {
            if (!attacker.IsObjectiveTarget)
            {
                return true;
            }
        }

        return false;
    }

    Snapshot BuildSnapshot(Player* bot, ObjectGuid const& objectiveTarget)
    {
        Snapshot snapshot;
        if (!bot)
        {
            return snapshot;
        }

        snapshot.BotInCombat = bot->IsInCombat();

        for (Unit* attackerUnit : bot->getAttackers())
        {
            if (!attackerUnit)
            {
                continue;
            }

            Attacker attacker;
            attacker.Guid = attackerUnit->GetGUID();
            attacker.Entry = attackerUnit->GetEntry();
            attacker.Distance = bot->GetDistance(attackerUnit);
            attacker.IsObjectiveTarget = (attacker.Guid == objectiveTarget);
            snapshot.Attackers.push_back(attacker);
        }

        return snapshot;
    }
} // namespace AutonomousPlayer::EncounterModel
