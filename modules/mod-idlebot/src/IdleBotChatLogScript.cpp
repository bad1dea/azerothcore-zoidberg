#include "IdleBotChatLogScript.h"
#include "IdleBotManager.h"
#include "IdleBotLog.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "SharedDefines.h"

// Verified in this checkout:
//   - PlayerScript ctor takes an optional enabled-hooks list (PlayerScript.h:229);
//     siblings pass PLAYERHOOK_CAN_PLAYER_USE_* (mod-playerbots Playerbots.cpp).
//   - Player::Say/Yell/TextEmote/Whisper invoke sScriptMgr->OnPlayerCanUseChat at
//     the server-side emit point (Player.cpp:9399+), so these hooks fire for
//     bot-originated chat too. The "Can" hooks are used here purely as observers
//     (always return true).
//
// KNOWN GAP: mod-playerbots' TellMaster to a real master sends a raw WorldPacket
// (PlayerbotAI.cpp:TellMasterNoFacing), bypassing Player::Whisper — those lines
// are NOT captured here. Master-less bots use bot->Say(), which IS captured.

namespace
{
    char const* ChatTypeLabel(uint32 type)
    {
        switch (type)
        {
            case CHAT_MSG_SAY:            return "SAY";
            case CHAT_MSG_YELL:           return "YELL";
            case CHAT_MSG_EMOTE:          return "EMOTE";
            case CHAT_MSG_TEXT_EMOTE:     return "EMOTE";
            case CHAT_MSG_WHISPER:        return "WHISPER";
            case CHAT_MSG_PARTY:          return "PARTY";
            case CHAT_MSG_PARTY_LEADER:   return "PARTY";
            case CHAT_MSG_RAID:           return "RAID";
            case CHAT_MSG_RAID_LEADER:    return "RAID";
            case CHAT_MSG_GUILD:          return "GUILD";
            case CHAT_MSG_CHANNEL:        return "CHANNEL";
            default:                      return "CHAT";
        }
    }

    void Capture(Player* player, uint32 type, std::string const& msg, std::string const& dest)
    {
        if (!player || !sIdleBotLog->CaptureChat())
            return;

        std::string const& name = player->GetName();
        if (!sIdleBotMgr->IsRegistered(name))
            return;

        std::string tag = std::string("CHAT/") + ChatTypeLabel(type);
        std::string line = dest.empty() ? msg : ("-> " + dest + ": " + msg);
        sIdleBotLog->Write(name, tag, line);
    }
}

class IdleBotChatLogScript : public PlayerScript
{
public:
    IdleBotChatLogScript() : PlayerScript("IdleBotChatLogScript",
    {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT
    }) { }

    // say / yell / emote
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg) override
    {
        Capture(player, type, msg, "");
        return true;
    }

    // whisper
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        Capture(player, type, msg, receiver ? receiver->GetName() : std::string());
        return true;
    }

    // party / raid
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* /*group*/) override
    {
        Capture(player, type, msg, "party");
        return true;
    }

    // guild
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* /*guild*/) override
    {
        Capture(player, type, msg, "guild");
        return true;
    }

    // channel
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* /*channel*/) override
    {
        Capture(player, type, msg, "channel");
        return true;
    }
};

void AddSC_idlebot_chatlog()
{
    new IdleBotChatLogScript();
}
