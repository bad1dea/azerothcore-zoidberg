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

#ifndef AUTONOMOUS_PLAYER_BOT_RECOVERY_H
#define AUTONOMOUS_PLAYER_BOT_RECOVERY_H

class Player;

namespace AutonomousPlayer::Recovery
{
    // Submits a real "release spirit" request on a dead `bot` via the
    // public opcode handler (WorldSession::HandleRepopRequestOpcode) a
    // game client uses when the player clicks the release-spirit button
    // on the death screen. This calls the real
    // Player::BuildPlayerRepop()/RepopAtGraveyard() -- core's own
    // legitimate death mechanic moves the ghost to the nearest graveyard;
    // that is not a TeleportTo call by this module (it's the same thing
    // that happens for any real player who releases spirit) and is not
    // disallowed by the player-like policy.
    //
    // Only valid while `bot` is dead and not already a ghost. Returns
    // true if the request was submitted (no guarantee of success --
    // same no-feedback-to-a-socketless-session caveat as every other
    // opcode-reuse function in this module). Verify with
    // bot->HasPlayerFlag(PLAYER_FLAGS_GHOST) afterward.
    bool RequestReleaseSpirit(Player* bot);

    // Submits a real corpse-reclaim (resurrect-at-corpse) request via the
    // public opcode handler (WorldSession::HandleReclaimCorpseOpcode) a
    // game client uses when right-clicking its own corpse. Requires,
    // exactly as for a real player: `bot` must be a ghost, its corpse
    // must exist, at least the real corpse-reclaim delay (~30s) must have
    // elapsed since release, and the ghost must be within
    // CORPSE_RECLAIM_RADIUS (39 yards) of the corpse -- this function
    // does not move the bot there itself (see Navigation::MoveTo, driven
    // by the caller/Planner once that exists) or wait out the delay; it
    // only submits the request, which core will silently reject (no
    // client to report the error to) if those conditions aren't met yet.
    //
    // Returns true if the request was submitted (no guarantee of
    // success). Verify with bot->IsAlive() afterward.
    bool RequestReclaimCorpse(Player* bot);

    // Spirit-healer resurrection (entry 6491 at every graveyard) via
    // the real opcode path -- sickness + durability cost apply for
    // real. The honest last resort for a corpse no ghost can walk to
    // (deep water, sealed geometry). Returns true if submitted;
    // verify with bot->IsAlive().
    bool RequestSpiritHealerResurrect(Player* bot);

    // Cast the hearthstone (item 6948, spell 8690) via the real
    // use-item opcode -- 10s cast, 60min cooldown, engine-validated.
    // The player-legitimate cross-continent recovery for a bot that
    // wandered onto a transport.
    bool RequestUseHearthstone(Player* bot);
} // namespace AutonomousPlayer::Recovery

#endif // AUTONOMOUS_PLAYER_BOT_RECOVERY_H
