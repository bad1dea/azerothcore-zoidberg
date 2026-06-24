#include "IdleBotCommandScript.h"
#include "IdleBotManager.h"
#include "IdleBotLog.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Configuration/Config.h"
#include "MotionMaster.h"
#include "Optional.h"
#include "Player.h"
#include "ScriptMgr.h"
#include <vector>

// Command API verified against this checkout:
//   - modules/mod-playerbots/src/Script/PlayerbotCommandScript.cpp
//   - src/server/scripts/Commands/cs_account.cpp (typed handler args)
//   - src/server/game/Chat/ChatCommands/ChatCommand.h:242 (uint32 security)
// Handlers are static members to match every command script in this checkout.

using namespace Acore::ChatCommands;

class idlebot_commandscript : public CommandScript
{
public:
    idlebot_commandscript() : CommandScript("idlebot_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        // Honor IdleBot.AllowGMOnly at registration time. Config is loaded before
        // scripts register, so sConfigMgr has real values here.
        static uint32 const sec =
            sConfigMgr->GetOption<bool>("IdleBot.AllowGMOnly", true) ? SEC_GAMEMASTER : SEC_PLAYER;

        static ChatCommandTable guideTable =
        {
            { "set",     HandleGuideSet,     sec, Console::No },
            { "clear",   HandleGuideClear,   sec, Console::No },
            { "current", HandleGuideCurrent, sec, Console::No },
            { "reset",   HandleGuideReset,   sec, Console::No },
            { "step",    HandleGuideStep,    sec, Console::No },
            { "list",    HandleGuideList,    sec, Console::No },
        };

        static ChatCommandTable idlebotTable =
        {
            { "help",    HandleHelp,    sec, Console::No },
            { "list",    HandleList,    sec, Console::No },
            { "add",     HandleAdd,     sec, Console::No },
            { "remove",  HandleRemove,  sec, Console::No },
            { "status",  HandleStatus,  sec, Console::No },
            { "summary", HandleSummary, sec, Console::No },
            { "log",     HandleLog,     sec, Console::No },
            { "goto",    HandleGoto,    sec, Console::No },
            { "teleport", HandleGoto,   sec, Console::No },
            { "gear",    HandleGear,    sec, Console::Yes },
            { "pause",   HandlePause,   sec, Console::No },
            { "resume",  HandleResume,  sec, Console::No },
            { "guide",   guideTable },
            { "",        HandleHelp,    sec, Console::No },   // bare ".idlebot" -> help
        };

        static ChatCommandTable base =
        {
            { "idlebot", idlebotTable }
        };

        return base;
    }

private:
    // SendSysMessage delivers a single line; split so multi-line summaries from
    // the manager render as separate chat lines rather than one run-on string.
    static void SendLines(ChatHandler* handler, std::string const& text)
    {
        std::string::size_type start = 0;
        while (start <= text.size())
        {
            std::string::size_type nl = text.find('\n', start);
            if (nl == std::string::npos)
            {
                handler->SendSysMessage(text.substr(start));
                break;
            }
            handler->SendSysMessage(text.substr(start, nl - start));
            start = nl + 1;
        }
    }

    static bool HandleHelp(ChatHandler* handler)
    {
        handler->SendSysMessage("IdleBot commands:");
        handler->SendSysMessage("  .idlebot list                       - list registered bots");
        handler->SendSysMessage("  .idlebot add <botName>              - register a bot");
        handler->SendSysMessage("  .idlebot remove <botName>           - unregister a bot");
        handler->SendSysMessage("  .idlebot status <botName>           - show a bot's status");
        handler->SendSysMessage("  .idlebot summary <botName>          - IdleRPG summary + last events");
        handler->SendSysMessage("  .idlebot log <botName> [lines]      - tail the bot's event log");
        handler->SendSysMessage("  .idlebot goto <botName>             - teleport yourself to a live bot");
        handler->SendSysMessage("  .idlebot gear <botName>             - force gear+spec at the bot's current level");
        handler->SendSysMessage("  .idlebot pause <botName>            - pause a bot");
        handler->SendSysMessage("  .idlebot resume <botName>           - resume a bot");
        handler->SendSysMessage("  .idlebot guide list [filter]        - list available guides");
        handler->SendSysMessage("  .idlebot guide set <botName> <id>   - assign a guide (skips done quests)");
        handler->SendSysMessage("  .idlebot guide clear <botName>      - clear current guide");
        handler->SendSysMessage("  .idlebot guide current <botName>    - show current guide step");
        handler->SendSysMessage("  .idlebot guide reset <botName>      - restart guide at step 1");
        handler->SendSysMessage("  .idlebot guide step <botName> <n>   - jump to step n (1-based)");
        handler->PSendSysMessage("Module is currently {}.",
            sIdleBotMgr->IsEnabled() ? "ENABLED" : "DISABLED (IdleBot.Enabled = 0)");
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        SendLines(handler, sIdleBotMgr->ListBots());
        return true;
    }

    static bool HandleAdd(ChatHandler* handler, std::string name)
    {
        std::string err;
        if (sIdleBotMgr->AddBot(name, err))
            handler->PSendSysMessage("Added bot {}.", name);
        else
            handler->PSendSysMessage("Add failed: {}", err);
        return true;
    }

    static bool HandleRemove(ChatHandler* handler, std::string name)
    {
        std::string err;
        if (sIdleBotMgr->RemoveBot(name, err))
            handler->PSendSysMessage("Removed bot {}.", name);
        else
            handler->PSendSysMessage("Remove failed: {}", err);
        return true;
    }

    static bool HandleStatus(ChatHandler* handler, std::string name)
    {
        SendLines(handler, sIdleBotMgr->StatusOf(name));
        return true;
    }

    static bool HandleSummary(ChatHandler* handler, std::string name)
    {
        SendLines(handler, sIdleBotMgr->SummaryOf(name));
        return true;
    }

    // .idlebot log <botName> [lines]   (default 15, capped at 50)
    static bool HandleLog(ChatHandler* handler, std::string name, Optional<uint32> lines)
    {
        uint32 count = lines ? *lines : 15;
        if (count == 0 || count > 50)
            count = 50;

        std::vector<std::string> tail = sIdleBotLog->Tail(name, count);
        if (tail.empty())
        {
            handler->PSendSysMessage("No log entries for {} (logging may be disabled).", name);
            return true;
        }
        handler->PSendSysMessage("Last {} log line(s) for {}:", uint32(tail.size()), name);
        for (std::string const& l : tail)
            handler->SendSysMessage(l);
        return true;
    }

    static bool HandleGoto(ChatHandler* handler, std::string name)
    {
        if (!handler->GetSession())
        {
            handler->SendSysMessage("This command must be run in game.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
        {
            handler->SendSysMessage("No active player session.");
            return true;
        }

        idlebot::BotPosition pos;
        std::string err;
        if (!sIdleBotMgr->GetLivePosition(name, pos, err))
        {
            handler->PSendSysMessage("IdleBot teleport failed: {}", err);
            return true;
        }

        if (player->IsInFlight())
        {
            player->GetMotionMaster()->MovementExpired();
            player->CleanupAfterTaxiFlight();
        }
        else
            player->SaveRecallPosition();

        if (player->TeleportTo(pos.mapId, pos.x, pos.y, pos.z + 0.25f, player->GetOrientation(), TELE_TO_GM_MODE))
            handler->PSendSysMessage("Teleported to {} at map {} ({:.1f}, {:.1f}, {:.1f}).", name, pos.mapId, pos.x, pos.y, pos.z);
        else
            handler->PSendSysMessage("IdleBot teleport failed: could not teleport to {}.", name);

        return true;
    }

    static bool HandlePause(ChatHandler* handler, std::string name)
    {
        if (sIdleBotMgr->PauseBot(name))
            handler->PSendSysMessage("Paused bot {}.", name);
        else
            handler->PSendSysMessage("No such bot: {}", name);
        return true;
    }

    static bool HandleResume(ChatHandler* handler, std::string name)
    {
        if (sIdleBotMgr->ResumeBot(name))
            handler->PSendSysMessage("Resumed bot {}.", name);
        else
            handler->PSendSysMessage("No such bot: {}", name);
        return true;
    }

    // .idlebot gear <botName>  — force gear+spec at the bot's current level
    static bool HandleGear(ChatHandler* handler, std::string name)
    {
        std::string err;
        if (sIdleBotMgr->GearBot(name, err))
            handler->PSendSysMessage("Bot {} geared + specced at its current level.", name);
        else
            handler->PSendSysMessage("Gear failed: {}", err);
        return true;
    }

    // .idlebot guide set <botName> <guideId>
    static bool HandleGuideSet(ChatHandler* handler, std::string botName, std::string guideId)
    {
        std::string err;
        if (sIdleBotMgr->SetGuide(botName, guideId, err))
            handler->PSendSysMessage("Bot {} guide set to '{}'.", botName, guideId);
        else
            handler->PSendSysMessage("Guide set failed: {}", err);
        return true;
    }

    // .idlebot guide clear <botName>
    static bool HandleGuideClear(ChatHandler* handler, std::string botName)
    {
        std::string err;
        if (sIdleBotMgr->ClearGuide(botName, err))
            handler->PSendSysMessage("Bot {} guide cleared.", botName);
        else
            handler->PSendSysMessage("Guide clear failed: {}", err);
        return true;
    }

    // .idlebot guide current <botName>
    static bool HandleGuideCurrent(ChatHandler* handler, std::string botName)
    {
        SendLines(handler, sIdleBotMgr->GuideCurrent(botName));
        return true;
    }

    // .idlebot guide reset <botName>
    static bool HandleGuideReset(ChatHandler* handler, std::string botName)
    {
        std::string err;
        if (sIdleBotMgr->GuideReset(botName, err))
            handler->PSendSysMessage("Bot {} guide reset to step 1.", botName);
        else
            handler->PSendSysMessage("Guide reset failed: {}", err);
        return true;
    }

    // .idlebot guide step <botName> <n>   (n is 1-based for the user)
    static bool HandleGuideStep(ChatHandler* handler, std::string botName, uint32 step)
    {
        if (step == 0)
        {
            handler->SendSysMessage("Step number is 1-based; use 1 or higher.");
            return true;
        }
        std::string err;
        if (sIdleBotMgr->SetGuideStep(botName, step - 1, err))
            handler->PSendSysMessage("Bot {} jumped to step {}.", botName, step);
        else
            handler->PSendSysMessage("Guide step failed: {}", err);
        return true;
    }

    // .idlebot guide list [faction|race]
    static bool HandleGuideList(ChatHandler* handler, Optional<std::string> filter)
    {
        auto const& guides = sIdleBotMgr->GetGuides();
        std::string filterStr = filter.value_or("");
        std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        uint32_t count = 0;
        handler->SendSysMessage("Available guides:");
        for (auto const& [id, guide] : guides)
        {
            if (!filterStr.empty())
            {
                std::string lower = id + " " + guide.faction + " " + guide.race;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find(filterStr) == std::string::npos)
                    continue;
            }
            handler->PSendSysMessage("  {} ({}, {}, L{}-{}, {} steps{})",
                id, guide.faction, guide.race,
                guide.levelMin, guide.levelMax,
                guide.steps.size(),
                guide.nextGuide.empty() ? "" : " → " + guide.nextGuide);
            ++count;
        }
        handler->PSendSysMessage("{} guide(s) found.", count);
        return true;
    }
};

void AddSC_idlebot_commandscript()
{
    new idlebot_commandscript();
}
