#ifndef MOD_IDLEBOT_COMMANDSCRIPT_H
#define MOD_IDLEBOT_COMMANDSCRIPT_H

// IdleBotCommandScript
// -----------------------------------------------------------------------------
// Registers the .idlebot chat command tree with AzerothCore's CommandScript.
//
// Implementation (IdleBotCommandScript.cpp) uses the modern Acore::ChatCommands
// API (ChatCommandTable + typed handler args), matching the pattern in
// modules/mod-playerbots/src/Script/PlayerbotCommandScript.cpp and core
// src/server/scripts/Commands/*.cpp in this checkout.
//
// Handlers delegate to sIdleBotMgr.

void AddSC_idlebot_commandscript();

#endif // MOD_IDLEBOT_COMMANDSCRIPT_H
