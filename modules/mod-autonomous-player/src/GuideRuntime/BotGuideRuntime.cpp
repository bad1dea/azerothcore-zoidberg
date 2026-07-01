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

#include "BotGuideRuntime.h"
#include "Navigation/BotNavigation.h"
#include "Player.h"

namespace AutonomousPlayer::GuideRuntime
{
    void Tick(Player* bot, BotGuideState& state)
    {
        if (!bot || state.Finished)
        {
            return;
        }

        if (state.CurrentStep >= state.Steps.size())
        {
            state.Finished = true;
            return;
        }

        GuideStep const& step = state.Steps[state.CurrentStep];

        switch (step.Type)
        {
            case StepType::MoveTo:
            {
                if (!state.ActionIssuedForCurrentStep)
                {
                    Navigation::MoveTo(bot, step.X, step.Y, step.Z);
                    state.ActionIssuedForCurrentStep = true;
                    return;
                }

                if (bot->GetDistance(step.X, step.Y, step.Z) <= ArrivalToleranceYards)
                {
                    ++state.CurrentStep;
                    state.ActionIssuedForCurrentStep = false;
                }

                break;
            }
        }
    }
} // namespace AutonomousPlayer::GuideRuntime
