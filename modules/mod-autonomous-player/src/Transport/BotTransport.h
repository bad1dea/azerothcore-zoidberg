#ifndef AUTONOMOUS_PLAYER_BOT_TRANSPORT_H
#define AUTONOMOUS_PLAYER_BOT_TRANSPORT_H

#include "Define.h"

class Player;
class Transport;

namespace AutonomousPlayer::TransportBehaviors
{
    // Find the spawned MotionTransport of `entry` on the bot's current map.
    // Returns nullptr if it isn't on this map (it may be mid-route on the other
    // side, or the bot is on the wrong map).
    Transport* FindTransport(Player* bot, uint32_t entry);

    // Is `transport` currently docked within `radius` of (x,y,z)? Used to tell
    // "arrived at the departure/destination dock" from "still travelling".
    bool TransportNear(Transport* transport, float x, float y, float z, float radius = 30.0f);

    // Attach the bot to the transport as a passenger (SetTransport +
    // AddPassenger) so it rides with it across the map boundary -- a bot has no
    // client to send the on-transport movement that normally does this, so
    // without it the bot is left behind / displaced. Returns false if already
    // aboard or inputs are invalid.
    bool BoardTransport(Player* bot, Transport* transport);

    // Detach the bot from its transport (RemovePassenger + SetTransport null).
    bool LeaveTransport(Player* bot);

    // True while the bot is attached to any transport.
    bool IsOnTransport(Player const* bot);
}

#endif
