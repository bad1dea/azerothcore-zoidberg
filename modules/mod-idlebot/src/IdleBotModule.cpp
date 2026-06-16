#include "IdleBotManager.h"
#include "IdleBotCommandScript.h"

// TODO(verify): AzerothCore script base includes.
//   #include "ScriptMgr.h"   -> WorldScript, ScriptMgr registration
//   #include "World.h"
//
// This file provides:
//   1. A WorldScript that initializes the manager and pumps its tick from
//      OnUpdate (the only safe place to advance bot state on the world thread).
//   2. The module's master AddSC_<module>() entry that AC's ScriptLoader calls.

// -----------------------------------------------------------------------------
// World hook
// -----------------------------------------------------------------------------
// TODO(verify): confirm WorldScript virtual names in your AC:
//   - OnStartup() / OnAfterConfigLoad(bool reload)
//   - OnUpdate(uint32 diff)
//   - OnShutdown()
//
// class IdleBotWorldScript : public WorldScript
// {
// public:
//     IdleBotWorldScript() : WorldScript("IdleBotWorldScript") {}
//
//     void OnAfterConfigLoad(bool /*reload*/) override
//     {
//         sIdleBotMgr->Initialize();
//     }
//
//     void OnUpdate(uint32 diff) override
//     {
//         sIdleBotMgr->OnWorldUpdate(diff);
//     }
//
//     void OnShutdown() override
//     {
//         sIdleBotMgr->Shutdown();
//     }
// };

// -----------------------------------------------------------------------------
// Module loader entry. AC's ScriptLoader (modules script_loader) calls this.
// TODO(verify): the naming convention AC expects for module loaders, e.g.
//   void Addmod_idlebotScripts()  OR  void AddSC_mod_idlebot()
// Match what other modules in your checkout use; the build will tell you.
// -----------------------------------------------------------------------------
void Addmod_idlebotScripts()
{
    // new IdleBotWorldScript();        // TODO: enable once WorldScript verified
    AddSC_idlebot_commandscript();
}
