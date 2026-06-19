#ifndef MOD_IDLEBOT_ZONEROUTE_H
#define MOD_IDLEBOT_ZONEROUTE_H

#include <cstdint>

namespace idlebot
{
    // A leveling hub: where a bot of a given faction should head when it has run
    // out of quests where it is, so it picks the work back up at the right place
    // for its level. Coordinates are the actual spawn position of the first quest
    // giver of the matching validated Zygor route (DB-derived, not hand-typed) —
    // see tools/zygor_*.py and data/routes/. This is a REFERENCE the organic
    // questing falls back on, not a script it must follow.
    struct LevelHub
    {
        uint8_t  faction;     // 0 = Alliance, 1 = Horde
        uint32_t minLevel;    // hub applies from this level up (until the next hub)
        uint32_t mapId;
        float    x, y, z;
        char const* zone;
        bool     raceNeutral; // safe to steer ANY same-faction race here (i.e. not
                              // a race-gated starting zone). Steering only uses
                              // neutral hubs so we never strand a mismatched race.
    };

    // Best STEERABLE hub for (faction, level): the highest minLevel race-neutral
    // entry <= level. Returns false if none (e.g. an unfilled level band — see
    // the 12-55 gap notes in NOTES.md).
    bool NextHubFor(uint8_t faction, uint32_t level, LevelHub& out);
}

#endif // MOD_IDLEBOT_ZONEROUTE_H
