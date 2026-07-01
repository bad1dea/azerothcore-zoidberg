/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AUTONOMOUS_PLAYER_BOT_LIFECYCLE_MGR_H
#define AUTONOMOUS_PLAYER_BOT_LIFECYCLE_MGR_H

#include "GuideRuntime/BotGuideRuntime.h"
#include "ObjectGuid.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace AutonomousPlayer
{
    // Per-bot bookkeeping owned by the lifecycle registry. Gate 3 slice 1
    // adds `Guide`, the first real (non-stub) per-bot behavior state --
    // everything before this was pure stagger/bookkeeping with no actual
    // dispatch.
    struct BotSession
    {
        ObjectGuid CharacterGuid;
        uint32_t AccumulatedMs = 0;
        uint32_t TickCount = 0;
        GuideRuntime::BotGuideState Guide;
    };

    // Registers active bots and drives them forward one tick at a time,
    // staggering per-bot work instead of doing it all on every world frame.
    //
    // This is a WorldScript owned singleton (see AutonomousPlayerModule.cpp)
    // so its Tick() call happens on the world thread inside OnUpdate --
    // never call it from anywhere else.
    class BotLifecycleMgr
    {
    public:
        static BotLifecycleMgr* Instance();

        // Registers a bot for ticking. No-op if already registered.
        void RegisterBot(ObjectGuid guid);

        // Removes a bot from the registry (logout, deletion, etc.).
        void UnregisterBot(ObjectGuid guid);

        [[nodiscard]] bool IsRegistered(ObjectGuid guid) const;
        [[nodiscard]] std::size_t GetBotCount() const;

        // Read-only enumeration of currently-registered bot GUIDs, for
        // status reporting (e.g. the .autonomousplayer status command).
        // Not for use by Planner/Executor -- those act on one bot's own
        // guid, resolved per tick (see ADR-002), not the whole registry.
        [[nodiscard]] std::vector<ObjectGuid> GetRegisteredBotGuids() const;

        // Advances every registered bot's accumulator by diff milliseconds.
        // When a bot's accumulator reaches TickIntervalMs, its TickCount is
        // incremented and the accumulator resets -- this is the stagger
        // mechanism (Gate 0 has no per-bot behavior to run yet; Gate 1 wires
        // this into an actual Planner call per elapsed tick).
        void Update(uint32_t diff);

        [[nodiscard]] uint32_t GetTickCount(ObjectGuid guid) const;

        // Attaches a fixed step list to a registered bot and starts
        // automatic execution -- no further manual command is needed;
        // Update() advances it on its own from here (Gate 3's "no manual
        // step advances" requirement). No-op if the bot isn't registered.
        void StartGuide(ObjectGuid guid, std::vector<GuideRuntime::GuideStep> steps);

        // Read-only status for the .autonomousplayer guidestatus command.
        // Returns nullptr if the bot isn't registered.
        [[nodiscard]] GuideRuntime::BotGuideState const* GetGuideState(ObjectGuid guid) const;

        static constexpr uint32_t TickIntervalMs = 1000;

    private:
        std::unordered_map<ObjectGuid, BotSession> _sessions;
    };
} // namespace AutonomousPlayer

#define sBotLifecycleMgr AutonomousPlayer::BotLifecycleMgr::Instance()

#endif // AUTONOMOUS_PLAYER_BOT_LIFECYCLE_MGR_H
