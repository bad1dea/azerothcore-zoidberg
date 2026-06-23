#ifndef MOD_IDLEBOT_ZONEROUTE_H
#define MOD_IDLEBOT_ZONEROUTE_H

#include <cstdint>

namespace idlebot
{
    // A leveling hub: where a bot of a given faction (and, for low levels, a given
    // race) should head when it has run out of quests where it is, so it picks the
    // work back up at the right place for its level. A REFERENCE the organic
    // questing falls back on when stalled, not a script it must follow.
    //
    // Low-level hubs (1-19) are race-specific: every playable race's starting zone
    // town and its second zone, so a stalled low-level bot is steered to ITS OWN
    // race's zone (not a mismatched starter). Coordinates are real DB spawn
    // positions (innkeepers / flight masters / playercreateinfo). From level ~20 up,
    // races funnel into shared contested zones, so those hubs are race-neutral
    // (race == 0 matches any race of the faction) — DB-derived from validated Zygor
    // routes (Outland/Northrend) and Horde/Alliance quest-giver centroids (vanilla).
    struct LevelHub
    {
        uint8_t  faction;     // 0 = Alliance, 1 = Horde
        uint8_t  race;        // 0 = any race of this faction; else a specific WoW race id
        uint32_t minLevel;    // hub applies from this level up (until the next hub)
        uint32_t mapId;
        float    x, y, z;
        char const* zone;
    };

    // Best hub for (faction, race, level): the highest minLevel entry <= level that
    // matches the faction and is either race-neutral (race == 0) or this bot's race.
    // A race-specific hub wins ties with a neutral hub at the same minLevel. Returns
    // false if none (e.g. an unfilled level band — see NOTES.md).
    bool NextHubFor(uint8_t faction, uint8_t race, uint32_t level, LevelHub& out);

    struct TransportRoute
    {
        uint32_t transportEntry;
        uint32_t dockMapId;
        float dockX, dockY, dockZ;
        uint32_t destMapId;
        float destX, destY, destZ;
        char const* name;
    };

    bool FindTransportRoute(uint32_t fromMap, uint32_t toMap, uint8_t teamId, TransportRoute& out);

    struct Waypoint
    {
        uint32_t mapId;
        float x, y, z;
        uint32_t areaTrigger;   // 0 = none; nonzero = fire this areatrigger on arrival
    };

    struct WaypointChain
    {
        char const* name;
        uint8_t teamId;          // 0=Alliance, 1=Horde, 2=any
        Waypoint const* points;
        uint32_t count;
    };

    // Find the nearest waypoint chain that connects (fromX,fromY on fromMap) toward
    // (toX,toY on toMap). Returns the chain and the starting waypoint index.
    bool FindWaypointChain(uint32_t fromMap, float fromX, float fromY,
                           uint32_t toMap, float toX, float toY,
                           uint8_t teamId,
                           WaypointChain const*& outChain, uint32_t& outStartIdx);
}

#endif // MOD_IDLEBOT_ZONEROUTE_H
