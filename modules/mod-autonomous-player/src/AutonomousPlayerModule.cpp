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
#include "Lifecycle/BotSessionMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Perception/PerceptionBuilder.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Setup/BotProvisioning.h"
#include "Setup/PendingCharacterCreations.h"
#include "Telemetry/Telemetry.h"
#include "WorldSession.h"

namespace
{
    bool ModuleEnabled = true;

    // How often (in BotLifecycleMgr ticks, i.e. roughly seconds) a
    // registered bot's perception snapshot is logged. This is the
    // "expose read-only" half of Gate 1's acceptance criteria -- the other
    // half is the on-demand `.autonomousplayer status` command.
    constexpr uint32_t PerceptionLogEveryNTicks = 10;
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
        sBotSessionMgr->Update(diff);
        AutonomousPlayer::Setup::PendingCharacterCreations::Update();

        for (ObjectGuid const& guid : sBotLifecycleMgr->GetRegisteredBotGuids())
        {
            uint32_t tickCount = sBotLifecycleMgr->GetTickCount(guid);
            if (tickCount == 0 || tickCount % PerceptionLogEveryNTicks != 0)
            {
                continue;
            }

            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player)
            {
                continue;
            }

            AutonomousPlayer::PerceptionSnapshot snapshot = AutonomousPlayer::BuildPerceptionSnapshot(player);
            LOG_INFO(AutonomousPlayer::Telemetry::LogCategory,
                "perception: {} lvl {} map {} pos ({:.1f}, {:.1f}, {:.1f}) hp {}/{} alive={} combat={} ghost={}",
                snapshot.CharacterName, snapshot.Level, snapshot.MapId,
                snapshot.PositionX, snapshot.PositionY, snapshot.PositionZ,
                snapshot.Health, snapshot.MaxHealth, snapshot.IsAlive, snapshot.IsInCombat,
                snapshot.IsGhost);
        }
    }
};

class AutonomousPlayerPlayerScript : public PlayerScript
{
public:
    AutonomousPlayerPlayerScript() : PlayerScript("AutonomousPlayerPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT
    })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        // WorldSession::IsBot() alone is not enough to identify "our" bots
        // -- mod-playerbots also sets it on its own random-bot sessions.
        // See ARCHITECTURE.md ADR-008 and Setup::IsAutonomousPlayerAccount.
        if (!ModuleEnabled || !player || !player->GetSession() || !player->GetSession()->IsBot()
            || !AutonomousPlayer::Setup::IsAutonomousPlayerAccount(player->GetSession()->GetAccountId()))
        {
            return;
        }

        sBotLifecycleMgr->RegisterBot(player->GetGUID());
        LOG_INFO(AutonomousPlayer::Telemetry::LogCategory,
            "bot '{}' ({}) logged in, {} bot(s) now registered.",
            player->GetName(), player->GetGUID().ToString(), sBotLifecycleMgr->GetBotCount());
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
        {
            return;
        }

        sBotLifecycleMgr->UnregisterBot(player->GetGUID());

        // Only queue-remove sessions this module owns (see
        // Setup::CreateBotSession) -- Playerbots' bots and real players
        // are not ours to delete. QueueForRemoval is a safe no-op if the
        // session isn't tracked by us.
        if (WorldSession* session = player->GetSession();
            session && session->IsBot()
            && AutonomousPlayer::Setup::IsAutonomousPlayerAccount(session->GetAccountId()))
        {
            sBotSessionMgr->QueueForRemoval(session);
        }
    }
};

class AutonomousPlayerUnitScript : public UnitScript
{
public:
    AutonomousPlayerUnitScript() : UnitScript("AutonomousPlayerUnitScript", true, {
        UNITHOOK_ON_DAMAGE
    }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!ModuleEnabled || !attacker || !victim || damage == 0)
            return;

        Player* attackingPlayer = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (attackingPlayer && sBotLifecycleMgr->IsRegistered(attackingPlayer->GetGUID()))
        {
            sBotLifecycleMgr->RecordDamage(
                attackingPlayer->GetGUID(), victim->GetGUID(), damage, true);
        }

        Player* victimPlayer = victim->ToPlayer();
        if (victimPlayer && sBotLifecycleMgr->IsRegistered(victimPlayer->GetGUID()))
        {
            sBotLifecycleMgr->RecordDamage(
                victimPlayer->GetGUID(), attacker->GetGUID(), damage, false);
        }
    }
};

void AddAutonomousPlayerScripts()
{
    new AutonomousPlayerConfig();
    new AutonomousPlayerWorld();
    new AutonomousPlayerPlayerScript();
    new AutonomousPlayerUnitScript();
}
