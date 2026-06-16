#include "IdleBotCommandScript.h"
#include "IdleBotManager.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Configuration/Config.h"
#include "ScriptMgr.h"

// Command API verified against this checkout:
//   - modules/mod-playerbots/src/Script/PlayerbotCommandScript.cpp
//   - src/server/scripts/Commands/cs_account.cpp (typed handler args)

using namespace Acore::ChatCommands;

namespace
{
    // SendSysMessage delivers a single line; split so multi-line summaries from
    // the manager render as separate chat lines rather than one run-on string.
    void SendLines(ChatHandler* handler, std::string const& text)
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

    bool HandleHelp(ChatHandler* handler)
    {
        handler->SendSysMessage("IdleBot commands:");
        handler->SendSysMessage("  .idlebot list                - list registered bots");
        handler->SendSysMessage("  .idlebot add <botName>       - register a bot");
        handler->SendSysMessage("  .idlebot remove <botName>    - unregister a bot");
        handler->SendSysMessage("  .idlebot status <botName>    - show a bot's status");
        handler->SendSysMessage("  .idlebot pause <botName>     - pause a bot");
        handler->SendSysMessage("  .idlebot resume <botName>    - resume a bot");
        handler->PSendSysMessage("Module is currently {}.", sIdleBotMgr->IsEnabled() ? "ENABLED" : "DISABLED (IdleBot.Enabled = 0)");
        return true;
    }

    bool HandleList(ChatHandler* handler)
    {
        SendLines(handler, sIdleBotMgr->ListBots());
        return true;
    }

    bool HandleAdd(ChatHandler* handler, std::string name)
    {
        std::string err;
        if (sIdleBotMgr->AddBot(name, err))
            handler->PSendSysMessage("Added bot {}.", name);
        else
            handler->PSendSysMessage("Add failed: {}", err);
        return true;
    }

    bool HandleRemove(ChatHandler* handler, std::string name)
    {
        std::string err;
        if (sIdleBotMgr->RemoveBot(name, err))
            handler->PSendSysMessage("Removed bot {}.", name);
        else
            handler->PSendSysMessage("Remove failed: {}", err);
        return true;
    }

    bool HandleStatus(ChatHandler* handler, std::string name)
    {
        SendLines(handler, sIdleBotMgr->StatusOf(name));
        return true;
    }

    bool HandlePause(ChatHandler* handler, std::string name)
    {
        if (sIdleBotMgr->PauseBot(name))
            handler->PSendSysMessage("Paused bot {}.", name);
        else
            handler->PSendSysMessage("No such bot: {}", name);
        return true;
    }

    bool HandleResume(ChatHandler* handler, std::string name)
    {
        if (sIdleBotMgr->ResumeBot(name))
            handler->PSendSysMessage("Resumed bot {}.", name);
        else
            handler->PSendSysMessage("No such bot: {}", name);
        return true;
    }
}

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

        static ChatCommandTable idlebotTable =
        {
            { "help",   HandleHelp,   sec, Console::No },
            { "list",   HandleList,   sec, Console::No },
            { "add",    HandleAdd,    sec, Console::No },
            { "remove", HandleRemove, sec, Console::No },
            { "status", HandleStatus, sec, Console::No },
            { "pause",  HandlePause,  sec, Console::No },
            { "resume", HandleResume, sec, Console::No },
            { "",       HandleHelp,   sec, Console::No },   // bare ".idlebot" -> help
        };

        static ChatCommandTable base =
        {
            { "idlebot", idlebotTable }
        };

        return base;
    }
};

void AddSC_idlebot_commandscript()
{
    new idlebot_commandscript();
}
