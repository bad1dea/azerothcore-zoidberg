#include "IdleBotTrainers.h"

namespace idlebot
{
    namespace
    {
        // Class ids (WoW 3.3.5a): WARRIOR=1 PALADIN=2 HUNTER=3 ROGUE=4 PRIEST=5
        // DEATHKNIGHT=6 SHAMAN=7 MAGE=8 WARLOCK=9 DRUID=11. Coordinates are real
        // class-trainer spawn positions pulled from the world DB (acore_world
        // creature/creature_template, subname '<Class> Trainer'). Map ids: 0=EK,
        // 1=Kalimdor, 530=Outland/Exodar.
        constexpr TrainerLoc kTrainers[] = {
            // --- Horde: Orgrimmar (Druid -> Thunder Bluff) ---
            { 1,  1,   1,   1980.0f,  -4799.7f,   56.1f, "Orgrimmar"     }, // Warrior
            { 1,  3,   1,   2085.0f,  -4623.8f,   58.9f, "Orgrimmar"     }, // Hunter
            { 1,  4,   1,   1771.2f,  -4284.4f,    8.1f, "Orgrimmar"     }, // Rogue
            { 1,  5,   1,   1452.4f,  -4179.8f,   44.4f, "Orgrimmar"     }, // Priest
            { 1,  7,   1,   1933.7f,  -4224.9f,   42.4f, "Orgrimmar"     }, // Shaman
            { 1,  8,   1,   1470.0f,  -4222.0f,   43.3f, "Orgrimmar"     }, // Mage
            { 1,  9,   1,   1844.2f,  -4353.6f,  -14.6f, "Orgrimmar"     }, // Warlock
            { 1, 11,   1,  -1039.4f,   -281.6f,  159.1f, "Thunder Bluff" }, // Druid
            // --- Alliance: Stormwind (Shaman -> The Exodar) ---
            { 0,  1,   0,  -8689.3f,    323.2f,  109.5f, "Stormwind"     }, // Warrior
            { 0,  2,   0,  -8574.0f,    860.9f,  106.6f, "Stormwind"     }, // Paladin
            { 0,  3,   0,  -8413.0f,    541.5f,  102.6f, "Stormwind"     }, // Hunter
            { 0,  4,   0,  -8752.3f,    377.6f,  101.1f, "Stormwind"     }, // Rogue
            { 0,  5,   0,  -8519.0f,    863.4f,  109.9f, "Stormwind"     }, // Priest
            { 0,  7,   0,  -3804.5f, -11396.9f, -104.3f, "The Exodar"    }, // Shaman
            { 0,  8,   0,  -8990.0f,    862.9f,   29.6f, "Stormwind"     }, // Mage
            { 0,  9,   0,  -8980.0f,   1041.1f,  101.5f, "Stormwind"     }, // Warlock
            { 0, 11,   0,  -8742.0f,   1095.4f,   93.8f, "Stormwind"     }, // Druid
        };
    }

    bool ClassTrainerLoc(uint8_t faction, uint8_t classId, TrainerLoc& out)
    {
        for (TrainerLoc const& t : kTrainers)
        {
            if (t.faction == faction && t.classId == classId)
            {
                out = t;
                return true;
            }
        }
        return false;
    }
}
