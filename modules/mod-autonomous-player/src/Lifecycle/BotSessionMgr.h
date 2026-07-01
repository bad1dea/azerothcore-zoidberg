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

#ifndef AUTONOMOUS_PLAYER_BOT_SESSION_MGR_H
#define AUTONOMOUS_PLAYER_BOT_SESSION_MGR_H

#include <cstdint>
#include <vector>

class WorldSession;

namespace AutonomousPlayer::Lifecycle
{
    // Owns and periodically ticks bot WorldSessions *outside* the core
    // WorldSessionMgr.
    //
    // Why: WorldSessionMgr::UpdateSessions deletes any session in its
    // registry whose Update() returns false -- and WorldSession::Update()
    // unconditionally returns false when m_Socket is null (see
    // ARCHITECTURE.md ADR-008). A sock=nullptr bot session registered via
    // sWorldSessionMgr->AddSession survives exactly one tick before being
    // destroyed, silently orphaning any async DB work still in flight
    // (character creation, login). mod-playerbots avoids this by never
    // registering its bot sessions with WorldSessionMgr at all -- it
    // drives them from its own update loop instead. This class is our
    // from-scratch equivalent of that technique (same public core API,
    // MapSessionFilter -- not a reuse of Playerbots' source): calling
    // WorldSession::Update(diff, MapSessionFilter) drains
    // ProcessQueryCallbacks() (so async DB chains progress) without ever
    // hitting the null-socket eviction path, because MapSessionFilter's
    // ProcessUnsafe() is false and that whole check is gated on
    // ProcessUnsafe() being true.
    class BotSessionMgr
    {
    public:
        static BotSessionMgr* Instance();

        // Starts ticking `session` every Update() call. No-op if already
        // tracked.
        void TrackSession(WorldSession* session);

        // Immediately stops ticking `session` and deletes it. Only safe to
        // call from a context that is NOT on `session`'s own call stack
        // (e.g. a provisioning command that owns the session outright).
        // Do NOT call this from a PlayerScript::OnPlayerLogout hook --
        // that fires from within the session's own logout/teardown call
        // stack, and deleting `session` there is a use-after-free once
        // control returns. Use QueueForRemoval instead in that case.
        void UntrackAndDelete(WorldSession* session);

        // Stops ticking `session` immediately (removed from the next
        // Update() pass) but defers the actual `delete` to the START of
        // the *next* Update() call, once the current call stack (e.g. a
        // logout hook) has fully unwound. Safe to call from
        // OnPlayerLogout.
        void QueueForRemoval(WorldSession* session);

        [[nodiscard]] std::size_t GetTrackedCount() const;

        // Deletes anything queued via QueueForRemoval on a prior call,
        // then calls WorldSession::Update(diff, MapSessionFilter) on every
        // still-tracked session. Must be called from the world thread (see
        // AutonomousPlayerWorld::OnUpdate).
        void Update(uint32_t diff);

    private:
        std::vector<WorldSession*> _sessions;
        std::vector<WorldSession*> _pendingRemoval;
    };
} // namespace AutonomousPlayer::Lifecycle

#define sBotSessionMgr AutonomousPlayer::Lifecycle::BotSessionMgr::Instance()

#endif // AUTONOMOUS_PLAYER_BOT_SESSION_MGR_H
