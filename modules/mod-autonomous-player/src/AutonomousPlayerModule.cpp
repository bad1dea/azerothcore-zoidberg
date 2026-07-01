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

#include "Config.h"
#include "Lifecycle/BotLifecycleMgr.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "Telemetry/Telemetry.h"

namespace
{
    bool ModuleEnabled = true;
}

class AutonomousPlayerConfig : public WorldScript
{
public:
    AutonomousPlayerConfig() : WorldScript("AutonomousPlayerConfig", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD
    })
    {
    }

    void OnBeforeConfigLoad(bool reload) override
    {
        if (reload)
        {
            return;
        }

        ModuleEnabled = sConfigMgr->GetOption<bool>("AutonomousPlayer.Enable", true);
    }
};

class AutonomousPlayerWorld : public WorldScript
{
public:
    AutonomousPlayerWorld() : WorldScript("AutonomousPlayerWorld", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    })
    {
    }

    void OnStartup() override
    {
        if (!ModuleEnabled)
        {
            LOG_INFO(AutonomousPlayer::Telemetry::LogCategory,
                "mod-autonomous-player: disabled via AutonomousPlayer.Enable, not starting.");
            return;
        }

        LOG_INFO(AutonomousPlayer::Telemetry::LogCategory,
            "mod-autonomous-player: loaded, Gate 0 scaffold, {} bots registered.",
            sBotLifecycleMgr->GetBotCount());
    }

    void OnUpdate(uint32 diff) override
    {
        if (!ModuleEnabled)
        {
            return;
        }

        sBotLifecycleMgr->Update(diff);
    }
};

void AddAutonomousPlayerScripts()
{
    new AutonomousPlayerConfig();
    new AutonomousPlayerWorld();
}
