#include "IdleBotManager.h"
#include "IdleBotCommandScript.h"
#include "IdleBotChatLogScript.h"
#include "ScriptMgr.h"

// WorldScript hook names verified in this checkout:
//   src/server/game/Scripting/ScriptDefines/WorldScript.h
//     OnAfterConfigLoad(bool reload), OnUpdate(uint32 diff), OnShutdown()
// Loader naming verified in modules/CMakeLists.txt (ConfigureScriptLoader):
//   "Add" + <dir name with '-' -> '_'> + "Scripts"  ->  Addmod_idlebotScripts()

// -----------------------------------------------------------------------------
// World hook: initialize the manager after config loads, pump its (non-blocking)
// tick from OnUpdate, and flush on shutdown.
// -----------------------------------------------------------------------------
class IdleBotWorldScript : public WorldScript
{
public:
    IdleBotWorldScript() : WorldScript("IdleBotWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sIdleBotMgr->Initialize();
    }

    void OnUpdate(uint32 diff) override
    {
        sIdleBotMgr->OnWorldUpdate(diff);
    }

    void OnShutdown() override
    {
        sIdleBotMgr->Shutdown();
    }
};

// -----------------------------------------------------------------------------
// Module loader entry. AC's generated ModulesLoader calls this.
// -----------------------------------------------------------------------------
void Addmod_idlebotScripts()
{
    new IdleBotWorldScript();
    AddSC_idlebot_commandscript();
    AddSC_idlebot_chatlog();
}
