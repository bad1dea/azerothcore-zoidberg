#ifndef MOD_IDLEBOT_TRAINERS_H
#define MOD_IDLEBOT_TRAINERS_H

#include <cstdint>

namespace idlebot
{
    // A class-trainer location: where a bot of a given faction+class can reliably
    // learn its new spells. These are real DB spawn coordinates of the class
    // trainers in the faction capitals (Orgrimmar / Thunder Bluff for Horde,
    // Stormwind / The Exodar for Alliance) — capitals always have the trainer plus
    // vendors and repair, so a deliberate "go to the city to train" trip (just like
    // a real player makes when they've been out questing a while) always succeeds.
    // Knowing where to go is reference data, NOT a per-quest script.
    struct TrainerLoc
    {
        uint8_t  faction;     // 0 = Alliance, 1 = Horde (matches GetTeamId)
        uint8_t  classId;     // WoW class id: WARRIOR=1 .. DRUID=11
        uint32_t mapId;
        float    x, y, z;
        char const* city;
    };

    // Look up the capital class-trainer location for (faction, classId). Returns
    // false for classes we don't have a pinned location for (e.g. Death Knights,
    // who start fully trained) — callers fall back to opportunistic in-town
    // training in that case.
    bool ClassTrainerLoc(uint8_t faction, uint8_t classId, TrainerLoc& out);
}

#endif // MOD_IDLEBOT_TRAINERS_H
