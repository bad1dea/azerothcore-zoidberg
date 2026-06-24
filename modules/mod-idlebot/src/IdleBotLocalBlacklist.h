#ifndef MOD_IDLEBOT_LOCALBLACKLIST_H
#define MOD_IDLEBOT_LOCALBLACKLIST_H

// IdleBotLocalBlacklist
// =============================================================================
// Per-bot, per-step GUID blacklist. Mirrors Honorbuddy's LocalBlacklist:
// when a target fails to yield the expected result (item not acquired after
// interact, mob unreachable, etc.) we blacklist its GUID for a short window
// so the bot moves on to the next available spawn instead of looping on the
// same broken target.
//
// HB defaults: 7-minute blacklist after looting; 90 seconds if another player
// is within NonCompeteDistance (25yd) of the target. We use tick-based expiry
// (each tick = IdleBot.TickMs, default 1000ms).
//
// The blacklist is GUID-keyed (not entry-keyed) because one entry's spawns may
// be healthy while another GUID is bugged, tapped, or already looted.
// It expires automatically on Tick() so stale entries never accumulate.
//
// Usage:
//   LocalBlacklist bl;
//   bl.Add(guid, /*expireAfterTicks=*/420);  // 420 ticks ≈ 7 min at 1s/tick
//   if (bl.IsBlacklisted(guid)) { ... skip ... }
//   bl.Tick();  // once per bot tick — expires old entries

#include <cstdint>
#include <unordered_map>

namespace idlebot
{
    class LocalBlacklist
    {
    public:
        void Add(uint64_t guid, uint32_t expireAfterTicks)
        {
            _entries[guid] = _tick + expireAfterTicks;
        }

        bool IsBlacklisted(uint64_t guid) const
        {
            auto it = _entries.find(guid);
            return it != _entries.end() && _tick < it->second;
        }

        void Tick()
        {
            ++_tick;
            for (auto it = _entries.begin(); it != _entries.end(); )
            {
                if (_tick >= it->second)
                    it = _entries.erase(it);
                else
                    ++it;
            }
        }

        void Clear()
        {
            _entries.clear();
        }

        bool Empty() const { return _entries.empty(); }
        std::size_t Size() const { return _entries.size(); }

    private:
        std::unordered_map<uint64_t, uint32_t> _entries;
        uint32_t _tick = 0;
    };
}

#endif // MOD_IDLEBOT_LOCALBLACKLIST_H
