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

#include "PerceptionBuilder.h"
#include "Corpse.h"
#include "Player.h"

namespace AutonomousPlayer
{
    PerceptionSnapshot BuildPerceptionSnapshot(Player const* player)
    {
        PerceptionSnapshot snapshot;

        if (!player)
        {
            return snapshot;
        }

        snapshot.CharacterGuid = player->GetGUID();
        snapshot.CharacterName = player->GetName();
        snapshot.Level = player->GetLevel();
        snapshot.MapId = player->GetMapId();
        snapshot.PositionX = player->GetPositionX();
        snapshot.PositionY = player->GetPositionY();
        snapshot.PositionZ = player->GetPositionZ();
        snapshot.Orientation = player->GetOrientation();
        snapshot.Health = player->GetHealth();
        snapshot.MaxHealth = player->GetMaxHealth();
        snapshot.IsAlive = player->IsAlive();
        snapshot.IsInCombat = player->IsInCombat();
        snapshot.IsGhost = player->HasPlayerFlag(PLAYER_FLAGS_GHOST);

        if (Corpse* corpse = player->GetCorpse())
        {
            snapshot.HasCorpse = true;
            snapshot.CorpseX = corpse->GetPositionX();
            snapshot.CorpseY = corpse->GetPositionY();
            snapshot.CorpseZ = corpse->GetPositionZ();
        }

        return snapshot;
    }
} // namespace AutonomousPlayer
