#include "IdleBotZoneRoute.h"
#include <array>

namespace idlebot
{
    namespace
    {
        // Ordered by (faction, minLevel). Auto-generated from validated Zygor
        // routes (first quest-giver DB coords). faction: 0=Alliance 1=Horde.
        // Regenerate via modules/mod-idlebot/tools (see NOTES.md).
        constexpr LevelHub kHubs[] = {
            { 0,   5,  530,   -4260.7f,  -13125.4f,    37.1f, "Azuremyst Isle" },
            { 0,   7,    0,   -9610.2f,   -1032.1f,    41.3f, "Elwynn Forest" },
            { 0,  11,  530,   -2670.2f,  -12131.4f,    17.2f, "Bloodmyst Isle" },
            { 0,  12,  530,   -4699.6f,  -12414.8f,    11.6f, "Bloodmyst Isle" },
            { 0,  55,    1,   -6847.8f,     755.7f,    42.2f, "Silithus" },
            { 0,  60,    0,  -11815.2f,   -3195.5f,   -30.9f, "Hellfire Peninsula" },
            { 0,  62,  530,    -215.5f,    5437.3f,    21.5f, "Zangarmarsh" },
            { 0,  64,  530,   -1560.2f,    5327.7f,    11.6f, "Terokkar Forest" },
            { 0,  65,  530,     974.3f,    7403.1f,    29.6f, "Nagrand" },
            { 0,  67,  530,     974.3f,    7403.1f,    29.6f, "Blade's Edge Mountains" },
            { 0,  68,  530,   -3918.0f,    2052.4f,    95.2f, "Shadowmoon Valley" },
            { 0,  70,  571,     593.1f,   -5089.0f,     5.6f, "Howling Fjord" },
            { 0,  72,  571,    3501.2f,    2000.6f,    65.7f, "Dragonblight" },
            { 0,  74,  571,    3410.8f,   -2781.7f,   202.3f, "Grizzly Hills" },
            { 0,  75,  571,    5151.9f,   -2199.2f,   236.6f, "Zul'Drak" },
            { 0,  77,  571,    5834.0f,     483.5f,   658.3f, "Sholazar Basin" },
            { 0,  78,  571,    6099.4f,   -1075.9f,   404.2f, "The Storm Peaks" },
            { 1,   1,  530,   10352.0f,   -6359.9f,    34.1f, "Blood Elf" },
            { 1,   5,  530,    9984.0f,   -6478.0f,     1.1f, "Eversong Woods" },
            { 1,  12,  530,    8118.0f,   -6901.5f,    70.4f, "Ghostlands" },
            { 1,  55,    1,   -6847.8f,     755.7f,    42.2f, "Silithus" },
            { 1,  60,    0,  -11817.3f,   -3187.9f,   -30.6f, "Hellfire Peninsula" },
            { 1,  62,  530,    -215.5f,    5437.3f,    21.5f, "Zangarmarsh" },
            { 1,  64,  530,   -1560.2f,    5327.7f,    11.6f, "Terokkar Forest" },
            { 1,  66,  530,     243.0f,    2698.2f,    89.8f, "Nagrand" },
            { 1,  67,  530,     928.5f,    5972.8f,   121.4f, "Blade's Edge Mountains" },
            { 1,  68,  530,   -3136.0f,    2550.0f,    62.3f, "Shadowmoon Valley" },
            { 1,  70,  571,    1948.5f,   -6146.6f,    24.3f, "Howling Fjord" },
            { 1,  71,  571,    3221.9f,    -691.4f,   167.2f, "Dragonblight" },
            { 1,  74,  571,    3200.6f,   -2301.5f,   107.8f, "Grizzly Hills" },
            { 1,  75,  571,    5151.9f,   -2199.2f,   236.6f, "Zul'Drak" },
            { 1,  77,  571,    5834.0f,     483.5f,   658.3f, "Sholazar Basin" },
            { 1,  78,  571,    6099.4f,   -1075.9f,   404.2f, "The Storm Peaks" },
        };
    }

    bool NextHubFor(uint8_t faction, uint32_t level, LevelHub& out)
    {
        bool found = false;
        for (LevelHub const& h : kHubs)
        {
            if (h.faction != faction)
                continue;
            if (h.minLevel > level)
                continue;
            if (!found || h.minLevel >= out.minLevel)
            {
                out = h;
                found = true;
            }
        }
        return found;
    }
}
