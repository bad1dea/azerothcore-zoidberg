#ifndef MOD_IDLEBOT_COMMANDSCRIPT_H
#define MOD_IDLEBOT_COMMANDSCRIPT_H

// IdleBotCommandScript
// -----------------------------------------------------------------------------
// Registers the .idlebot chat command tree with AzerothCore's CommandScript.
//
// TODO(verify): AzerothCore's command API. Confirm against your checkout:
//   - base class: CommandScript
//   - return type of GetCommands(): ChatCommandTable / std::vector<ChatCommand>
//   - the command table struct shape (it changed across AC versions; the modern
//     one uses Acore::ChatCommands::ChatCommandBuilder). Match a sibling module's
//     command script exactly rather than trusting memory.
//
// The .cpp implements handlers that call into sIdleBotMgr.

void AddSC_idlebot_commandscript();  // TODO(verify): AC loader naming convention

#endif // MOD_IDLEBOT_COMMANDSCRIPT_H
