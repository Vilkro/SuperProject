/* Tracking.h
   Server-side vertical-velocity / rocket-jump logger and scorer.

   Export mechanism: CORE_API (defined in Core/Core.h, matches Core/API/IChat.h etc.)
   Calling convention: plain functions, primitive/const-char* params only, matches
   PlayerDB.h's shape - see the .cpp for why CORE_API (not PlayerDB's plain extern)
   is required here specifically.

   ─── scoring model ──────────────────────────────────────────────────────────────
   - Primary score = net_height_diff: landing height minus launch height (i.e. the
     height of the wall/object actually reached and stuck).
   - Secondary = apex_height: the highest point reached during the flight, which can
     exceed net_height_diff if the arc goes over something and comes back down lower.
   - A jump is only "scored" (pbScored out-param, and the DB's scored column) if:
       hp_spent < rjt_fMaxHPSpent (self-damage taken during the flight)
       AND net_height_diff > rjt_fMinHeightDiff
       AND at least one explosion kick occurred (rocket or cannonball) - a plain
       hill-climb with zero kicks was still occasionally passing the other two gates
       on its own, so this is enforced directly rather than relying on them alone.
     Unscored jumps still get a DB row (useful for debugging/tuning), just not
     announced and excluded from leaderboard queries by filtering on scored=1.
   - Ground/liftoff/landing detection is plain en_penReference != NULL. Two different
     velocity-based landing refinements were tried and reverted (a near-zero-velocity
     requirement broke on slope-walking; a "was falling last tick" requirement broke a
     different way - see below). Separately, the LAUNCH baseline itself was redesigned:
     it used to be a rolling snapshot rewritten every grounded tick (ambiguous - which
     tick's read survives depends on how long the player stood there), now it's written
     exactly once, at the precise liftoff tick, sourced from the same "last known"
     fields already used for teleport detection (which update unconditionally every
     tick). One deterministic read, not a possibly-many-ticks-old one. See
     Player_es_integration.md section 1 for the exact field-role split. apex_height
     still freezes at first touch of anything structurally, since RJT_UpdateFlightPeak
     stops being called the instant en_penReference goes non-NULL again. The mid-ascent-
     graze problem (a wall/ledge touch while still rising ending the flight early) is
     real and still open - plain detection is the known baseline until a fix for that
     doesn't also break height accuracy.
   - Peak vertical velocity (peak_vertical_velocity) only counts the *ascending*
     phase and includes any jump-key stacking bonus. angle_hit_error is a separate,
     narrower figure: the angle (degrees) between the FIRST explosion kick's
     direction and straight up - 0 = hit dead-center under the player for a pure
     vertical push, larger = more off-angle, wasting some of the push horizontally.
     Independent of the "press jump late" stacking technique entirely.
   - Peak velocity/apex height are updated from TWO places: the regular per-tick
     hook (stage 3 in Player.es) AND directly inside RJT_LogKick using values
     captured at the moment of the kick itself. The latter exists because the
     per-tick hook reads velocity after that tick's gravity has already been
     subtracted once, undercounting the true peak by ~1 tick of decay (this
     mattered enough to be reported as a specific, measurable ~1.5 m/s
     discrepancy) - RJT_LogKick's capture is guaranteed undiminished for explosion
     kicks, since DamageImpact runs synchronously right after the impulse, before
     that tick's gravity integration. A plain jump-key kick doesn't have that
     luxury - it's applied inside the engine's own PreMovingNew(), which also does
     gravity for the same tick before returning, so Player.es's jump-kick capture
     (stage 2) is STILL one tick behind and needs an explicit correction:
     + en_fGravityA*TickQuantum, added back to reconstruct the undiminished value.
     Verified against real jump-combo test data: peak velocity was consistently
     ~1.5 m/s low specifically on jumps that used the jump-key stacking technique,
     while pure-explosion peaks (no jump-key) matched independently-derived
     theoretical values to within 0.001 m/s without needing this correction.
   - RJT_BeginFlight only carries forward kicks logged before the liftoff edge (the
     "shoot rocket while grounded, then jump late" case) if they're recent (within
     rjt's hardcoded 1s window) - an unrelated older explosion from actual prior
     combat would already be far outside that window by the time of a new jump, so
     the time check alone covers what an earlier "must be at ~full HP first"
     requirement was also trying to catch. That HP requirement got removed - it was
     silently blocking this whole mechanism (and everything built on it) any time
     the player wasn't at ~100 HP before jumping, which is the common case for
     repeated testing or normal play without healing back up between attempts.
     fHealthBeforeLaunch stays in RJT_BeginFlight's signature but is unused now,
     kept only so Player.es's call site doesn't need touching. When kicks ARE
     carried forward, the launch baseline itself also prefers the position captured
     synchronously at the first of those kicks (RJT_LogKick's fPosX/Y/Z/
     fHeightAtKick) over whatever Player.es's own "position one tick before liftoff"
     tracking passed in - en_penReference staying non-NULL between the kick and the
     eventual liftoff doesn't guarantee position stayed frozen that whole time, so
     the later read can already be measurably contaminated by however far the kick
     had already pushed them.
   - Iron cannonballs fire 4 near-simultaneous DMT_CANNONBALL_EXPLOSION sub-blasts,
     Nuke fires 13 - RJT_CommitFlight coalesces consecutive same-type hits within
     ~1 tick of each other into a single counted explosion, and keeps that count
     SEPARATE from other explosions (pctExplosions vs pctCannonballs) rather than
     lumping them into one total.
   - Kicks (both explosion hits AND the plain jump-key kick) are logged with a
     shared timeline so scoring queries can measure the gap between an explosion
     kick and a following jump-key kick - the "shoot, then jump as late as
     possible" technique.
   - Chat opt-out is NOT tracked in this file - it lives in PlayerDB
     (PlayerDB_GetAnnounceOptOut/SetAnnounceOptOut), consistent with how
     GetLanguage/SetLanguage already persist per-player preferences by GUID. */

#ifndef TRACKING_H
#define TRACKING_H

#ifdef PRAGMA_ONCE
#pragma once
#endif

     // ─── lifecycle ────────────────────────────────────────────────────────────────
     // Call from ClassicsPatch_Init() / ClassicsPatch_Shutdown() in Core/Core.cpp.
CORE_API void RJT_Init(void);
CORE_API void RJT_Shutdown(void);

// ─── events, called unconditionally from Player.es ─────────────────────────────
// All of these no-op internally if rjt_bEnable is off.
//
// iPlayer: en_ulID cast to INDEX (see prior note - not PlayerDB's slot-index
// convention, Player.es doesn't have that lookup available at these hook sites).

CORE_API void RJT_BeginFlight(INDEX iPlayer, const char* strPlayerName,
    float fLaunchX, float fLaunchY, float fLaunchZ, double tmLaunch,
    const char* strLaunchSurface, float fLaunchSurfaceHeight, float fHealthBeforeLaunch);

// fVerticalVelocity: signed, positive = up. Only positive values update the peak -
// pass the raw signed value every tick, Core does the ">0 and is it a new max" check.
CORE_API void RJT_UpdateFlightPeak(INDEX iPlayer, float fVerticalVelocity, float fHeightGained);

// Discards whatever flight is in progress for this player WITHOUT committing it to
// the DB - call this instead of the normal Begin/Update/End sequence when Player.es's
// own teleport sanity check trips (position jumped further than velocity could explain).
CORE_API void RJT_AbortFlight(INDEX iPlayer);

// One row per kick, explosion OR plain jump-key press, sharing one timeline so
// scoring can measure the gap between them.
// bIsJumpKick=1: this is the plain jump-key kick, not damage - iDamageType/
//   fDamageAmount/fKickDamage/fDirX-Z are meaningless, pass 0.
// bIsJumpKick=0: an explosion/impact hit - pass the raw DamageType enum value
//   (INDEX-cast), post-falloff damage, DamageImpact()'s kick multiplier, and
//   vDirectionFixed. Only called for DMT_EXPLOSION / DMT_CANNONBALL_EXPLOSION /
//   DMT_IMPACT hits per the integration guide.
// fVerticalDeltaV: (vVelAfter - vVelBefore) projected onto -en_vGravityDir, computed
// by the caller - Core has no knowledge of the world's gravity vector.
// fAngleCos: dot product of vDirectionFixed and -en_vGravityDir (both unit vectors,
// so this is directly cos(angle) with no extra normalization) - 1.0 = explosion was
// dead-center under the player (straight upward push), 0.0 = a 90-degree hit with no
// vertical push at all. Meaningless for jump-key kicks (bIsJumpKick=1), pass 0.
// Used to derive "angle hit error" in degrees at commit time - see RJT_EndFlight.
// fVerticalVelNow/fHeightNow: current vertical velocity/height gained, captured by
// the caller AT THE SAME MOMENT as the rest of this kick's data - for explosion kicks
// this is immediately after GiveImpulseTranslationAbsolute, guaranteed undiminished by
// gravity for this tick (the later per-tick peak-tracking hook reads velocity after
// that tick's gravity has already been subtracted once, undercounting the true peak
// by one tick's decay). RJT_LogKick updates the peak trackers directly from these,
// even before RJT_BeginFlight() has fired - a pre-liftoff explosion (the "shoot
// rocket, jump late" technique) needs this, since a separate peak-update call would
// silently no-op with no active flight yet.
// fPosX/Y/Z, fHeightAtKick: absolute world position and height (along -en_vGravityDir)
// at this SAME synchronous moment. Only meaningful/used for non-jump kicks. If this is
// the FIRST kick logged for a not-yet-active flight (a pre-liftoff explosion), Core
// remembers this exact position as the candidate TRUE launch baseline - since
// en_penReference staying non-NULL doesn't mean position is frozen (proven by the
// slope-walking case), "one tick before liftoff" can already be measurably
// contaminated by a kick that landed a tick or more earlier. RJT_BeginFlight prefers
// this captured position over whatever Player.es's own last-known-position tracking
// passed in, whenever a pre-liftoff kick is being carried forward into the new flight.
CORE_API void RJT_LogKick(INDEX iPlayer, double tmTick, INDEX iDamageType, INDEX bIsJumpKick,
    float fDamageAmount, float fKickDamage,
    float fDirX, float fDirY, float fDirZ,
    float fVelBeforeX, float fVelBeforeY, float fVelBeforeZ,
    float fVelAfterX, float fVelAfterY, float fVelAfterZ,
    float fVerticalDeltaV, float fAngleCos, float fVerticalVelNow, float fHeightNow,
    float fPosX, float fPosY, float fPosZ, float fHeightAtKick);

// fLandHeight: landing position's height along -en_vGravityDir, same convention as
// fLaunchSurfaceHeight in RJT_BeginFlight - Core computes net_height_diff from the
// two, it doesn't know gravity direction itself.
// pbValid: FALSE if there was no active flight to end (tracker was off, or this
//   player's flight was already aborted as a teleport).
// pbScored: Core's scoring decision (hp_spent/height-diff gates) - independent of
//   whether chat announcing is even enabled.
// pbShouldAnnounce: pbScored AND rjt_bAnnounceInChat - what Player.es should
//   actually act on for the chat line.
// pfAngleHitErrorDeg: angle (degrees) between the FIRST explosion kick's direction
//   and straight up, i.e. how far off dead-center the rocket hit was - 0 = perfect,
//   larger = worse. -1 if the flight had no explosion kick before its first (or only)
//   jump-key kick, e.g. a plain jump with nothing to rate.
// pctExplosions: non-cannonball explosions (rockets, impacts, etc.)
// pctCannonballs: cannonball explosions specifically, kept separate - 4 (Iron) / 13
//   (Nuke) sub-blasts within ~1 tick of each other already grouped into 1 each.
CORE_API void RJT_EndFlight(INDEX iPlayer, double tmLand,
    float fLandX, float fLandY, float fLandZ, float fLandHeight, const char* strLandSurface,
    INDEX* pbValid, INDEX* pbScored, INDEX* pbShouldAnnounce,
    float* pfAirtime, float* pfApexHeight, float* pfNetHeightDiff, float* pfPeakVerticalVelocity,
    float* pfAngleHitErrorDeg, INDEX* pctExplosions, INDEX* pctCannonballs, float* pfHPSpent);

// Call periodically (e.g. CoreTimerHandler::OnSecond) to clean up flights abandoned
// by disconnect/crash rather than a clean landing or a caught teleport.
CORE_API void RJT_ExpireStaleFlights(double tmNow, double fMaxAirtime);

// Player.es can't read rjt_fTeleportThreshold directly (it's Core-local data, same
// reason as the others) - use this instead of hardcoding the threshold.
CORE_API float RJT_GetTeleportThreshold(void);

// Broadcasts a jump announcement to all connected clients, EN/RU per recipient, no
// "Server:" prefix. Reimplements SayToAllExceptLang's exact loop (Core/Networking/
// NetworkFunctions.cpp) rather than calling it directly - that function takes
// SHELL_FUNC_ARGS (stack-based variadic calling convention for the shell/scripting
// system), which isn't safely callable with normal C++ call syntax from Player.es.
// Also needs PlayerDB_GetLanguage() and PlayerDB_GetAnnounceOptOut() (both
// Core/Query/PlayerDB.h), Core-local for the same cross-DLL reason as everything else
// here - this wrapper is what makes the whole thing reachable from Player.es in one call.
// PlayerDB_GetAnnounceOptOut() itself is NOT part of this file - see
// PlayerDB_AnnounceOptOut_patch.md for the actual PlayerDB.h/.cpp changes needed,
// consistent with SetLanguage/GetLanguage's persistent GUID-keyed storage.
CORE_API void RJT_AnnounceJump(const char* strMessageEN, const char* strMessageRU);

// VERIFY: check an existing Core header for the real export macro this project
// uses if Core builds as a DLL (something like CORE_API/DECL_DLL) - plain
// declaration below assumes static linking, adjust if the compiler complains.
CORE_API void NotifyCoopMarkerActivated(const char* strMessageEN, const char* strMessageRU);

void ReportStuckState(CEntity* penPlayer, BOOL bCurrentlyStuck, const char* strReasonEN, const char* strReasonRU);
void CheckFallTime(CEntity* penPlayer, FLOAT fFallTime);

// ─── config, for Core.cpp's DeclareSymbol() calls ONLY ──────────────────────
// Do NOT reference these from EntitiesTSE/Player.es. Every scoring decision is
// made in Core and handed back via RJT_EndFlight's out-params.
extern INDEX rjt_bEnable;
extern CTString rjt_strDBPath;
extern INDEX rjt_bAnnounceInChat;
extern FLOAT rjt_fMinHeightDiff;   // meters - net_height_diff must exceed this to score
extern FLOAT rjt_fMaxHPSpent;      // scored only if hp_spent stays under this (default 100)
extern FLOAT rjt_fTeleportThreshold; // meters/tick of unexplained displacement = teleport

#endif // TRACKING_H