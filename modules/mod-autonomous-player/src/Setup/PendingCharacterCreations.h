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

#ifndef AUTONOMOUS_PLAYER_PENDING_CHARACTER_CREATIONS_H
#define AUTONOMOUS_PLAYER_PENDING_CHARACTER_CREATIONS_H

#include <cstdint>
#include <string>

class WorldSession;

namespace AutonomousPlayer::Setup
{
    // Tracks bot-provisioning sessions that have an in-flight
    // SubmitCharacterCreate request, so AutonomousPlayerWorld::OnUpdate can
    // keep polling sCharacterCache for completion and clean the throwaway
    // session up (via Lifecycle::BotSessionMgr) once it's done -- either
    // because the character now exists, or because it timed out.
    //
    // This class owns nothing engine-side by itself; it just remembers
    // "session S is waiting for character C to appear" so the completion
    // check and cleanup can happen from the world tick instead of the
    // one-shot admin command handler that submitted the request.
    namespace PendingCharacterCreations
    {
        // Starts watching `session` for `characterName` to appear in the
        // character cache. `session` must already be tracked with
        // Lifecycle::BotSessionMgr::TrackSession.
        void Watch(WorldSession* session, std::string const& characterName);

        // Checks every watched creation: if the character now exists,
        // logs success and cleans the session up
        // (Lifecycle::BotSessionMgr::UntrackAndDelete); if it's been
        // waiting more than TimeoutTicks calls to this function, logs a
        // failure and cleans up the same way. Call once per
        // AutonomousPlayerWorld::OnUpdate tick.
        void Update();

        // How many Update() calls (roughly one per world frame) to keep
        // actively polling before giving up and logging an error. NOT a
        // safety-critical value -- Update() never deletes the session on
        // timeout (see the .cpp), only on confirmed completion, so a too-
        // short value here just means more log noise for genuinely slow
        // (but still eventually successful) creations, not a use-after-
        // free. Measured live on zoidberg: a chained character-creation
        // request can legitimately take well over 30s to complete on a
        // busy server (hundreds of Playerbots contending for the same DB
        // worker thread pool), so this is set generously.
        inline constexpr uint32_t TimeoutTicks = 6000;
    } // namespace PendingCharacterCreations
} // namespace AutonomousPlayer::Setup

#endif // AUTONOMOUS_PLAYER_PENDING_CHARACTER_CREATIONS_H
