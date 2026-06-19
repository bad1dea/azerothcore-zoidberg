#include "IdleBotZoneRoute.h"

namespace idlebot
{
    namespace
    {
        // faction: 0=Alliance 1=Horde. raceNeutral=false marks race-gated starter
        // zones (we never STEER a bot there — it might be the wrong race). The 55+
        // Outland/Northrend hubs are auto-generated from validated Zygor routes
        // (first quest-giver DB coords). The low-level race-neutral leveling hubs
        // (Silverpine/Hillsbrad …) are flight-master coords from the world DB and
        // fill the 12-55 gap the Cata Zygor guides don't cover. Regenerate the
        // Zygor-derived rows via modules/mod-idlebot/tools (see NOTES.md).
        constexpr LevelHub kHubs[] = {
            // --- race-gated starter zones: NOT steering targets ---
            { 0,   5,  530,   -4260.7f,  -13125.4f,    37.1f, "Azuremyst Isle",  false },
            { 0,   7,    0,   -9610.2f,   -1032.1f,    41.3f, "Elwynn Forest",   false },
            { 0,  11,  530,   -2670.2f,  -12131.4f,    17.2f, "Bloodmyst Isle",  false },
            { 1,   1,  530,   10352.0f,   -6359.9f,    34.1f, "Blood Elf",       false },
            { 1,   5,  530,    9984.0f,   -6478.0f,     1.1f, "Eversong Woods",  false },
            { 1,  12,  530,    8118.0f,   -6901.5f,    70.4f, "Ghostlands",      false },
            // --- race-neutral leveling hubs (DB flight-master coords) ---
            // Horde 12-52. L12/L20 are hand-verified EK towns (Sepulcher,
            // Tarren Mill); L24-52 are derive_hubs.py output — centroids of
            // Horde-ONLY quest givers per level (territory-safe by construction,
            // even if cross-continent). Fills the gap the Cata Zygor guides miss.
            { 1,  12,    0,     473.9f,    1533.9f,   132.0f, "Silverpine Forest", true },
            { 1,  20,    0,       2.7f,    -857.9f,    58.9f, "Hillsbrad Foothills", true },
            { 1,  24,    1,    3345.8f,    1021.0f,     4.7f, "L24 hub", true },
            { 1,  28,    1,    -438.0f,   -3176.0f,   211.0f, "L28 hub", true },
            { 1,  32,    0,     -32.8f,    -931.6f,    56.4f, "L32 hub", true },
            { 1,  36,    1,   -3127.9f,   -2863.3f,    34.5f, "L36 hub", true },
            { 1,  40,    0,    -959.2f,   -3534.4f,    67.5f, "L40 hub", true },
            { 1,  44,    1,   -4361.9f,     230.9f,    27.0f, "L44 hub", true },
            { 1,  48,    0,    -587.8f,   -4616.6f,    13.1f, "L48 hub", true },
            { 1,  52,    1,    3947.0f,   -1046.0f,   246.0f, "L52 hub", true },
            // Alliance 12-52, derive_hubs.py --strict (Alliance-only quest givers,
            // territory-safe). L12/16 land in Bloodmyst (Draenei) — Alliance-safe.
            { 0,  12,  530,   -1960.2f,  -11839.0f,    55.3f, "L12 hub", true },
            { 0,  16,  530,   -1962.5f,  -11865.4f,    51.9f, "L16 hub", true },
            { 0,  20,    0,   -9253.9f,   -2215.4f,    65.9f, "L20 hub", true },
            { 0,  24,    0,  -10545.1f,   -1174.1f,    28.3f, "L24 hub", true },
            { 0,  28,    0,  -10559.9f,   -1159.6f,    28.6f, "L28 hub", true },
            { 0,  32,    0,    -832.6f,    -570.7f,    13.1f, "L32 hub", true },
            { 0,  36,    1,   -3784.3f,   -4565.9f,    17.3f, "L36 hub", true },
            { 0,  40,    1,     177.9f,    1253.0f,   175.6f, "L40 hub", true },
            { 0,  44,    1,   -4345.0f,    3313.0f,    10.0f, "L44 hub", true },
            { 0,  48,    1,   -4441.6f,    3234.4f,    23.0f, "L48 hub", true },
            { 0,  52,    0,     955.7f,   -1429.4f,    64.9f, "L52 hub", true },
            // --- 55+ neutral (Outland + Northrend), Zygor-derived ---
            { 0,  55,    1,   -6847.8f,     755.7f,    42.2f, "Silithus",        true },
            { 0,  60,    0,  -11815.2f,   -3195.5f,   -30.9f, "Hellfire Peninsula", true },
            { 0,  62,  530,    -215.5f,    5437.3f,    21.5f, "Zangarmarsh",     true },
            { 0,  64,  530,   -1560.2f,    5327.7f,    11.6f, "Terokkar Forest", true },
            { 0,  65,  530,     974.3f,    7403.1f,    29.6f, "Nagrand",         true },
            { 0,  67,  530,     974.3f,    7403.1f,    29.6f, "Blade's Edge Mountains", true },
            { 0,  68,  530,   -3918.0f,    2052.4f,    95.2f, "Shadowmoon Valley", true },
            { 0,  70,  571,     593.1f,   -5089.0f,     5.6f, "Howling Fjord",   true },
            { 0,  72,  571,    3501.2f,    2000.6f,    65.7f, "Dragonblight",    true },
            { 0,  74,  571,    3410.8f,   -2781.7f,   202.3f, "Grizzly Hills",   true },
            { 0,  75,  571,    5151.9f,   -2199.2f,   236.6f, "Zul'Drak",        true },
            { 0,  77,  571,    5834.0f,     483.5f,   658.3f, "Sholazar Basin",  true },
            { 0,  78,  571,    6099.4f,   -1075.9f,   404.2f, "The Storm Peaks", true },
            { 1,  55,    1,   -6847.8f,     755.7f,    42.2f, "Silithus",        true },
            { 1,  60,    0,  -11817.3f,   -3187.9f,   -30.6f, "Hellfire Peninsula", true },
            { 1,  62,  530,    -215.5f,    5437.3f,    21.5f, "Zangarmarsh",     true },
            { 1,  64,  530,   -1560.2f,    5327.7f,    11.6f, "Terokkar Forest", true },
            { 1,  66,  530,     243.0f,    2698.2f,    89.8f, "Nagrand",         true },
            { 1,  67,  530,     928.5f,    5972.8f,   121.4f, "Blade's Edge Mountains", true },
            { 1,  68,  530,   -3136.0f,    2550.0f,    62.3f, "Shadowmoon Valley", true },
            { 1,  70,  571,    1948.5f,   -6146.6f,    24.3f, "Howling Fjord",   true },
            { 1,  71,  571,    3221.9f,    -691.4f,   167.2f, "Dragonblight",    true },
            { 1,  74,  571,    3200.6f,   -2301.5f,   107.8f, "Grizzly Hills",   true },
            { 1,  75,  571,    5151.9f,   -2199.2f,   236.6f, "Zul'Drak",        true },
            { 1,  77,  571,    5834.0f,     483.5f,   658.3f, "Sholazar Basin",  true },
            { 1,  78,  571,    6099.4f,   -1075.9f,   404.2f, "The Storm Peaks", true },
        };
    }

    bool NextHubFor(uint8_t faction, uint32_t level, LevelHub& out)
    {
        bool found = false;
        for (LevelHub const& h : kHubs)
        {
            if (h.faction != faction || !h.raceNeutral)
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
