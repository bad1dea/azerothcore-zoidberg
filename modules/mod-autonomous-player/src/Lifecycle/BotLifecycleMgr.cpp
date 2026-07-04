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

#include "BotLifecycleMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"

namespace AutonomousPlayer
{
    BotLifecycleMgr* BotLifecycleMgr::Instance()
    {
        static BotLifecycleMgr instance;
        return &instance;
    }

    void BotLifecycleMgr::RegisterBot(ObjectGuid guid)
    {
        _sessions.try_emplace(guid, BotSession{ guid, 0, 0 });
    }

    void BotLifecycleMgr::UnregisterBot(ObjectGuid guid)
    {
        _sessions.erase(guid);
    }

    bool BotLifecycleMgr::IsRegistered(ObjectGuid guid) const
    {
        return _sessions.contains(guid);
    }

    std::size_t BotLifecycleMgr::GetBotCount() const
    {
        return _sessions.size();
    }

    std::vector<ObjectGuid> BotLifecycleMgr::GetRegisteredBotGuids() const
    {
        std::vector<ObjectGuid> guids;
        guids.reserve(_sessions.size());

        for (auto const& [guid, session] : _sessions)
        {
            guids.push_back(guid);
        }

        return guids;
    }

    void BotLifecycleMgr::Update(uint32_t diff)
    {
        for (auto& [guid, session] : _sessions)
        {
            session.AccumulatedMs += diff;

            bool fired = false;
            while (session.AccumulatedMs >= TickIntervalMs)
            {
                session.AccumulatedMs -= TickIntervalMs;
                ++session.TickCount;
                fired = true;
            }

            // Resolved fresh every fire, never stored -- ADR-002's
            // tick-safety rule. The bot may have logged out/despawned
            // since it was registered; GuideRuntime::Tick/TickAmbient
            // both no-op on null.
            if (fired)
            {
                Player* player = ObjectAccessor::FindPlayer(guid);

                // Unconditional -- background bot maintenance (ADR-042)
                // runs regardless of guide state, closing the real gap
                // where a fully idle bot between guides got no pet
                // maintenance at all (KNOWN_FAILURES.md #16). Skip
                // Tick() for this same fire if it issued something --
                // otherwise a cast TickAmbient just started (e.g. Revive
                // Pet) could be interrupted immediately by this same
                // tick's guide-step dispatch (e.g. KillNearest's
                // Selecting phase finding a brand new target right
                // after). This restores the exact pause-by-skipping
                // behavior the pre-ADR-042 single-function version had
                // for free via a shared early-return.
                //
                // `ConsecutiveAmbientSkips` (`KNOWN_FAILURES.md` #19) is
                // the systemic backstop: a dead bot repeatedly attempting
                // a doomed instant-fail pet-recovery cast was one real,
                // found way this pause could persist forever with no
                // bounded-wait escape (fixed at the source too, in
                // `Recovery::PlanPetRecovery`/`PlanPetAcquisition`'s own
                // `IsAlive()` checks) -- this counter guards against any
                // other future persistent-failure mode having the same
                // effect, by forcing `Tick()` to run anyway once the cap
                // is hit.
                bool ambientActed = GuideRuntime::TickAmbient(player, session.Guide);
                session.ConsecutiveAmbientSkips = ambientActed ? (session.ConsecutiveAmbientSkips + 1) : 0;

                bool forceTickAnyway = session.ConsecutiveAmbientSkips > MaxConsecutiveAmbientSkips;
                if ((!ambientActed || forceTickAnyway) && !session.Guide.Finished)
                {
                    GuideRuntime::Tick(player, session.Guide);
                }
            }
        }
    }

    uint32_t BotLifecycleMgr::GetTickCount(ObjectGuid guid) const
    {
        auto it = _sessions.find(guid);
        return it != _sessions.end() ? it->second.TickCount : 0;
    }

    void BotLifecycleMgr::StartGuide(ObjectGuid guid, std::vector<GuideRuntime::GuideStep> steps)
    {
        auto it = _sessions.find(guid);
        if (it == _sessions.end())
        {
            return;
        }

        it->second.Guide = GuideRuntime::BotGuideState{};
        it->second.Guide.Steps = std::move(steps);

        // A stale count from before this guide started (e.g. from a
        // dead/idle period with no guide running at all) shouldn't count
        // against the fresh guide's own safety margin -- found via
        // self-review while fixing KNOWN_FAILURES.md #19, not itself a
        // hang risk (a premature force-tick is always harmless), just a
        // real correctness gap worth closing.
        it->second.ConsecutiveAmbientSkips = 0;
    }

    GuideRuntime::BotGuideState const* BotLifecycleMgr::GetGuideState(ObjectGuid guid) const
    {
        auto it = _sessions.find(guid);
        return it != _sessions.end() ? &it->second.Guide : nullptr;
    }

    void BotLifecycleMgr::RecordDamage(ObjectGuid botGuid, ObjectGuid otherGuid,
        uint32_t damage, bool outgoing)
    {
        auto it = _sessions.find(botGuid);
        if (it == _sessions.end() || damage == 0)
            return;

        GuideRuntime::BotGuideState& guide = it->second.Guide;
        if (outgoing)
        {
            if (guide.CurrentTargetGuid == otherGuid)
                guide.OutgoingDamage += damage;
        }
        else
        {
            guide.IncomingDamage += damage;
        }
    }
} // namespace AutonomousPlayer
