#include "IdleBotZoneRoute.h"

namespace idlebot
{
    namespace
    {
        // WoW race ids: HUMAN=1 ORC=2 DWARF=3 NIGHTELF=4 UNDEAD=5 TAUREN=6 GNOME=7
        // TROLL=8 BLOODELF=10 DRAENEI=11. faction: 0=Alliance 1=Horde.
        //
        // 1-19 rows are race-specific (race != 0): each race's starting-zone town
        // (innkeeper coords / playercreateinfo) at L1 and its second zone (flight-
        // master coords) at L10, so a stalled low-level bot is steered to ITS OWN
        // race's zone. From L20 up, races share contested zones, so hubs are
        // race-neutral (race == 0): L20-52 are derive_hubs.py centroids of
        // faction-only quest givers (territory-safe), L55-78 are validated-Zygor
        // first-quest-giver coords (Outland + Northrend). Regenerate the derived
        // rows via modules/mod-idlebot/tools (see NOTES.md).
        constexpr LevelHub kHubs[] = {
            // ===== Alliance starting + second zones (race-specific, 1-19) =====
            { 0,  1,  1,   0,  -9462.7f,     16.2f,    57.0f, "Elwynn (Goldshire)"     }, // Human
            { 0,  1, 10,   0, -10628.3f,   1037.3f,    34.2f, "Westfall"               },
            { 0,  3,  1,   0,  -5601.6f,   -531.2f,   399.7f, "Dun Morogh (Kharanos)"  }, // Dwarf
            { 0,  3, 10,   0,  -5424.9f,  -2929.9f,   347.6f, "Loch Modan"             },
            { 0,  7,  1,   0,  -5601.6f,   -531.2f,   399.7f, "Dun Morogh (Kharanos)"  }, // Gnome (shares Dwarf)
            { 0,  7, 10,   0,  -5424.9f,  -2929.9f,   347.6f, "Loch Modan"             },
            { 0,  4,  1,   1,  10127.9f,   2224.8f,  1328.8f, "Teldrassil (Dolanaar)"  }, // Night Elf
            { 0,  4, 10,   1,   6343.2f,    561.7f,    16.1f, "Darkshore"              },
            { 0, 11,  1, 530,  -4129.4f, -12469.0f,    44.2f, "Azuremyst (Azure Watch)"}, // Draenei
            { 0, 11, 10, 530,  -1930.0f, -11956.8f,    57.5f, "Bloodmyst Isle"         },
            // ===== Horde starting + second zones (race-specific, 1-19) =====
            { 1,  2,  1,   1,    340.4f,  -4686.3f,    16.5f, "Durotar (Razor Hill)"   }, // Orc
            { 1,  2, 10,   1,   -437.1f,  -2596.0f,    95.9f, "The Barrens (Crossroads)"},
            { 1,  8,  1,   1,    340.4f,  -4686.3f,    16.5f, "Durotar (Razor Hill)"   }, // Troll (shares Orc)
            { 1,  8, 10,   1,   -437.1f,  -2596.0f,    95.9f, "The Barrens (Crossroads)"},
            { 1,  5,  1,   0,   2269.5f,    244.9f,    34.3f, "Tirisfal (Brill)"       }, // Undead
            { 1,  5, 10,   0,    473.9f,   1533.9f,   132.0f, "Silverpine Forest"      },
            { 1,  6,  1,   1,  -2365.4f,   -347.3f,    -8.9f, "Mulgore (Bloodhoof)"    }, // Tauren
            { 1,  6, 10,   1,   -437.1f,  -2596.0f,    95.9f, "The Barrens (Crossroads)"},
            { 1, 10,  1, 530,   9565.7f,  -7222.8f,    16.4f, "Eversong (Falconwing)"  }, // Blood Elf
            { 1, 10, 10, 530,   8118.0f,  -6901.5f,    70.4f, "Ghostlands"             },
            // ===== Horde neutral leveling hubs (20-52) =====
            { 1,  0, 20,   0,      2.7f,   -857.9f,    58.9f, "Hillsbrad Foothills"    },
            { 1,  0, 24,   1,   3345.8f,   1021.0f,     4.7f, "L24 hub"                },
            { 1,  0, 28,   1,   -438.0f,  -3176.0f,   211.0f, "L28 hub"                },
            { 1,  0, 32,   0,    -32.8f,   -931.6f,    56.4f, "L32 hub"                },
            { 1,  0, 36,   1,  -3127.9f,  -2863.3f,    34.5f, "L36 hub"                },
            { 1,  0, 40,   0,   -959.2f,  -3534.4f,    67.5f, "L40 hub"                },
            { 1,  0, 44,   1,  -4361.9f,    230.9f,    27.0f, "L44 hub"                },
            { 1,  0, 48,   0,   -587.8f,  -4616.6f,    13.1f, "L48 hub"                },
            { 1,  0, 52,   1,   3947.0f,  -1046.0f,   246.0f, "L52 hub"                },
            // ===== Alliance neutral leveling hubs (20-52) =====
            { 0,  0, 20,   0,  -9253.9f,  -2215.4f,    65.9f, "L20 hub"                },
            { 0,  0, 24,   0, -10545.1f,  -1174.1f,    28.3f, "L24 hub"                },
            { 0,  0, 28,   0, -10559.9f,  -1159.6f,    28.6f, "L28 hub"                },
            { 0,  0, 32,   0,   -832.6f,   -570.7f,    13.1f, "L32 hub"                },
            { 0,  0, 36,   1,  -3784.3f,  -4565.9f,    17.3f, "L36 hub"                },
            { 0,  0, 40,   1,    177.9f,   1253.0f,   175.6f, "L40 hub"                },
            { 0,  0, 44,   1,  -4345.0f,   3313.0f,    10.0f, "L44 hub"                },
            { 0,  0, 48,   1,  -4441.6f,   3234.4f,    23.0f, "L48 hub"                },
            { 0,  0, 52,   0,    955.7f,  -1429.4f,    64.9f, "L52 hub"                },
            // ===== 55+ neutral (Outland + Northrend), Zygor-derived =====
            { 0,  0, 55,   1,  -6847.8f,    755.7f,    42.2f, "Silithus"               },
            { 0,  0, 60,   0, -11815.2f,  -3195.5f,   -30.9f, "Hellfire Peninsula"     },
            { 0,  0, 62, 530,   -215.5f,   5437.3f,    21.5f, "Zangarmarsh"            },
            { 0,  0, 64, 530,  -1560.2f,   5327.7f,    11.6f, "Terokkar Forest"        },
            { 0,  0, 65, 530,    974.3f,   7403.1f,    29.6f, "Nagrand"                },
            { 0,  0, 67, 530,    974.3f,   7403.1f,    29.6f, "Blade's Edge Mountains" },
            { 0,  0, 68, 530,  -3918.0f,   2052.4f,    95.2f, "Shadowmoon Valley"      },
            { 0,  0, 70, 571,    593.1f,  -5089.0f,     5.6f, "Howling Fjord"          },
            { 0,  0, 72, 571,   3501.2f,   2000.6f,    65.7f, "Dragonblight"           },
            { 0,  0, 74, 571,   3410.8f,  -2781.7f,   202.3f, "Grizzly Hills"          },
            { 0,  0, 75, 571,   5151.9f,  -2199.2f,   236.6f, "Zul'Drak"               },
            { 0,  0, 77, 571,   5834.0f,    483.5f,   658.3f, "Sholazar Basin"         },
            { 0,  0, 78, 571,   6099.4f,  -1075.9f,   404.2f, "The Storm Peaks"        },
            { 1,  0, 55,   1,  -6847.8f,    755.7f,    42.2f, "Silithus"               },
            { 1,  0, 60,   0, -11817.3f,  -3187.9f,   -30.6f, "Hellfire Peninsula"     },
            { 1,  0, 62, 530,   -215.5f,   5437.3f,    21.5f, "Zangarmarsh"            },
            { 1,  0, 64, 530,  -1560.2f,   5327.7f,    11.6f, "Terokkar Forest"        },
            { 1,  0, 66, 530,    243.0f,   2698.2f,    89.8f, "Nagrand"                },
            { 1,  0, 67, 530,    928.5f,   5972.8f,   121.4f, "Blade's Edge Mountains" },
            { 1,  0, 68, 530,  -3136.0f,   2550.0f,    62.3f, "Shadowmoon Valley"      },
            { 1,  0, 70, 571,   1948.5f,  -6146.6f,    24.3f, "Howling Fjord"          },
            { 1,  0, 71, 571,   3221.9f,   -691.4f,   167.2f, "Dragonblight"           },
            { 1,  0, 74, 571,   3200.6f,  -2301.5f,   107.8f, "Grizzly Hills"          },
            { 1,  0, 75, 571,   5151.9f,  -2199.2f,   236.6f, "Zul'Drak"               },
            { 1,  0, 77, 571,   5834.0f,    483.5f,   658.3f, "Sholazar Basin"         },
            { 1,  0, 78, 571,   6099.4f,  -1075.9f,   404.2f, "The Storm Peaks"        },
        };
    }

    bool NextHubFor(uint8_t faction, uint8_t race, uint32_t level, LevelHub& out)
    {
        bool found = false;
        for (LevelHub const& h : kHubs)
        {
            if (h.faction != faction)
                continue;
            if (h.race != 0 && h.race != race)   // race-specific hub for another race
                continue;
            if (h.minLevel > level)
                continue;
            // Highest minLevel <= level wins; on a tie prefer the race-specific hub
            // (race != 0) over a neutral one so low-level bots get their own zone.
            if (!found
                || h.minLevel > out.minLevel
                || (h.minLevel == out.minLevel && h.race != 0 && out.race == 0))
            {
                out = h;
                found = true;
            }
        }
        return found;
    }

    namespace
    {
        // teamId: 0 = Alliance (TEAM_ALLIANCE), 1 = Horde (TEAM_HORDE)
        // Dock positions are on the platform where players stand to board.
        // Dest positions are where the bot walks to after disembarking.
        constexpr TransportRoute kRoutes[] = {
            // Alliance: EK (map 0) → Kalimdor (map 1) via Stormwind Harbor → Auberdine
            { 176310, 0, -8643.f, 1330.f, 6.f,   1, 6443.f, 413.f, 9.f, "The Bravery (SW→Auberdine)" },
            // Alliance: Kalimdor (map 1) → EK (map 0) via Auberdine → Stormwind Harbor
            { 176310, 1, 6443.f, 413.f, 9.f,      0, -8643.f, 1330.f, 6.f, "The Bravery (Auberdine→SW)" },

            // Horde: EK (map 0) → Kalimdor (map 1) via UC zeppelin → Orgrimmar
            { 164871, 0, 2054.f, 242.f, 100.f,    1, 1331.f, -4649.f, 54.f, "Thundercaller (UC→Org)" },
            // Horde: Kalimdor (map 1) → EK (map 0) via Orgrimmar zeppelin → UC
            { 164871, 1, 1331.f, -4649.f, 54.f,   0, 2054.f, 242.f, 100.f, "Thundercaller (Org→UC)" },
        };
    }

    bool FindTransportRoute(uint32_t fromMap, uint32_t toMap, uint8_t teamId, TransportRoute& out)
    {
        for (auto const& r : kRoutes)
        {
            if (r.dockMapId == fromMap && r.destMapId == toMap)
            {
                bool const alliance = (teamId == 0);
                bool const isAllianceRoute = (r.transportEntry == 176310);
                if (alliance == isAllianceRoute)
                {
                    out = r;
                    return true;
                }
            }
        }
        return false;
    }

    namespace
    {
        // Alliance: Dun Morogh / Loch Modan → Ironforge → Deeprun Tram → Stormwind → Harbor
        // Covers the full journey from anywhere in the dwarf starting zones.
        constexpr Waypoint kAllianceDwarfToSWHarbor[] = {
            { 0, -6240.f, 331.f, 383.f, 0 },     // Coldridge Valley
            { 0, -6075.f, 314.f, 396.f, 0 },     // Anvilmar road south
            { 0, -5751.f, -196.f, 393.f, 0 },    // Kharanos (central Dun Morogh)
            { 0, -5610.f, -493.f, 402.f, 0 },    // Kharanos south road
            { 0, -5606.f, -513.f, 402.f, 0 },    // Thelsamar/South Gate junction
            { 0, -5400.f, -628.f, 397.f, 0 },    // South Gate Pass
            { 0, -5187.f, -782.f, 390.f, 0 },    // Dun Morogh road to IF
            { 0, -4981.f, -917.f, 504.f, 0 },    // Ironforge entrance exterior
            { 0, -4838.f, -1152.f, 502.f, 0 },   // Ironforge Great Forge area
            { 0, -4840.f, -1330.f, 508.f, 2175 }, // Deeprun Tram entrance (IF→tram)
            { 369, 69.f, 10.f, -4.f, 0 },         // Inside tram (IF side platform)
            { 369, 68.f, 2491.f, -4.f, 2171 },    // Inside tram (SW side → exit)
            { 0, -8364.f, 536.f, 92.f, 0 },       // Stormwind tram exit
            { 0, -8560.f, 645.f, 97.f, 0 },       // Stormwind Dwarven District
            { 0, -8643.f, 1330.f, 6.f, 0 },       // Stormwind Harbor dock
        };

        // Horde: Tirisfal Glades → Undercity → UC Zeppelin Tower
        constexpr Waypoint kHordeTirisfalToUCZep[] = {
            { 0, 1676.f, 1678.f, 122.f, 0 },     // Deathknell
            { 0, 2259.f, 276.f, 35.f, 0 },       // Brill area
            { 0, 2054.f, 242.f, 100.f, 0 },      // UC Zeppelin Tower
        };

        constexpr WaypointChain kChains[] = {
            { "Dun Morogh → SW Harbor", 0, kAllianceDwarfToSWHarbor,
              sizeof(kAllianceDwarfToSWHarbor) / sizeof(Waypoint) },
            { "Tirisfal → UC Zeppelin", 1, kHordeTirisfalToUCZep,
              sizeof(kHordeTirisfalToUCZep) / sizeof(Waypoint) },
        };
    }

    bool FindWaypointChain(uint32_t fromMap, float fromX, float fromY,
                           uint32_t toMap, float toX, float toY,
                           uint8_t teamId,
                           WaypointChain const*& outChain, uint32_t& outStartIdx)
    {
        float bestDist = 1e12f;
        outChain = nullptr;
        outStartIdx = 0;

        for (auto const& chain : kChains)
        {
            if (chain.teamId != 2 && chain.teamId != teamId)
                continue;

            // Does this chain's last waypoint get us closer to the destination?
            auto const& last = chain.points[chain.count - 1];
            if (last.mapId != toMap && fromMap == toMap)
                continue;

            // Find the nearest waypoint in this chain to our current position.
            for (uint32_t i = 0; i < chain.count; ++i)
            {
                auto const& wp = chain.points[i];
                if (wp.mapId != fromMap)
                    continue;
                float dx = wp.x - fromX, dy = wp.y - fromY;
                float d = dx * dx + dy * dy;
                if (d < bestDist)
                {
                    bestDist = d;
                    outChain = &chain;
                    outStartIdx = i;
                }
            }
        }
        return outChain != nullptr;
    }
}
