/* Tracking.cpp
   See Tracking.h for the scoring model. Must start with StdH.h like every
   other .cpp in this project (precompiled header requirement). */

#include "StdH.h"
#include "Tracking/Tracking.h"

   // Reusing the sqlite3.h you already have in Core/Query rather than a project-wide
   // include path - adjust this relative path if yours lives somewhere else under Query/.
#include "../Query/sqlite3.h"
// For PlayerDB_GetLanguage() - adjust this relative path if PlayerDB.h lives
// somewhere other than directly in Core/Query/.
#include "../Query/PlayerDB.h"
#include <cmath>
#include <map>
#include <vector>
#include <Networking/NetworkFunctions.h>

// ─── config, Core-local only - never read directly from EntitiesTSE ───────────
INDEX rjt_bEnable = 1;
CTString rjt_strDBPath = CTString("RocketJumps.db");
INDEX rjt_bAnnounceInChat = 1;
FLOAT rjt_fMinHeightDiff = 3.0f;
FLOAT rjt_fMaxHPSpent = 100.0f;
FLOAT rjt_fTeleportThreshold = 15.0f;

// ─── internal state ─────────────────────────────────────────────────────────

struct SKickRecord {
    DOUBLE tick;
    INDEX  iDamageType;
    INDEX  bIsJumpKick;
    FLOAT  fDamageAmount;
    FLOAT  fKickDamage;
    FLOAT  fDirX, fDirY, fDirZ;
    FLOAT  fVelBeforeX, fVelBeforeY, fVelBeforeZ;
    FLOAT  fVelAfterX, fVelAfterY, fVelAfterZ;
    FLOAT  fVerticalDeltaV;
    FLOAT  fAngleCos;
};

struct SFlightState {
    BOOL    bActive;
    CTString strPlayerName;

    DOUBLE  tmLaunch;
    FLOAT   fLaunchX, fLaunchY, fLaunchZ;
    CTString strLaunchSurface;
    FLOAT   fLaunchSurfaceHeight;

    FLOAT   fApexHeightGained;
    FLOAT   fPeakVerticalVelocity;   // ascending only - see RJT_UpdateFlightPeak

    // Position at the FIRST non-jump kick logged this buffering session - candidate
    // true launch baseline for the pre-liftoff-explosion case. Set once, never
    // overwritten by later kicks in the same session - see RJT_LogKick/RJT_BeginFlight.
    BOOL    bHasFirstKickPosition;
    FLOAT   fFirstKickX, fFirstKickY, fFirstKickZ, fFirstKickHeight;

    std::vector<SKickRecord> aKicks;

    SFlightState() : bActive(FALSE), tmLaunch(0), fLaunchX(0), fLaunchY(0), fLaunchZ(0),
        fLaunchSurfaceHeight(0), fApexHeightGained(0), fPeakVerticalVelocity(0),
        bHasFirstKickPosition(FALSE), fFirstKickX(0), fFirstKickY(0), fFirstKickZ(0),
        fFirstKickHeight(0) {
    }
};

static sqlite3* _pRJT_DB = NULL;
static std::map<INDEX, SFlightState> _mapRJT_ActiveFlights;

static const char* _strRJT_Schema =
"CREATE TABLE IF NOT EXISTS jumps ("
"  jump_id INTEGER PRIMARY KEY AUTOINCREMENT,"
"  player_id INTEGER NOT NULL,"
"  player_name TEXT NOT NULL,"
"  launch_tick REAL NOT NULL,"
"  launch_x REAL, launch_y REAL, launch_z REAL,"
"  launch_surface TEXT,"
"  launch_surface_height REAL,"
"  land_tick REAL,"
"  land_x REAL, land_y REAL, land_z REAL,"
"  land_surface TEXT,"
"  airtime REAL,"
"  apex_height REAL,"              // highest point reached (secondary score)
"  net_height_diff REAL,"          // land height - launch height (PRIMARY score)
"  peak_vertical_velocity REAL,"   // ascending-only peak (includes jump-key stacking)
"  angle_hit_error REAL,"          // degrees off dead-center on the first explosion, 0=perfect
"  explosion_count INTEGER DEFAULT 0,"      // non-cannonball explosions
"  cannonball_count INTEGER DEFAULT 0,"     // cannonball explosions, sub-blasts grouped
"  hp_spent REAL DEFAULT 0,"       // sum of damage taken during the flight
"  scored INTEGER DEFAULT 0"       // 1 = passed hp_spent/height-diff gates
");"
"CREATE TABLE IF NOT EXISTS kicks ("
"  kick_id INTEGER PRIMARY KEY AUTOINCREMENT,"
"  jump_id INTEGER NOT NULL REFERENCES jumps(jump_id),"
"  tick REAL NOT NULL,"
"  is_jump_kick INTEGER DEFAULT 0," // 1 = plain jump-key press, not an explosion
"  damage_type INTEGER NOT NULL,"
"  damage_amount REAL NOT NULL,"
"  kick_damage REAL NOT NULL,"
"  dir_x REAL, dir_y REAL, dir_z REAL,"
"  vel_before_x REAL, vel_before_y REAL, vel_before_z REAL,"
"  vel_after_x REAL, vel_after_y REAL, vel_after_z REAL,"
"  vertical_delta_v REAL"
");"
"CREATE INDEX IF NOT EXISTS idx_kicks_jump ON kicks(jump_id);";

static void RJT_ExecOrWarn(const char* strSQL) {
    char* strErr = NULL;
    if (sqlite3_exec(_pRJT_DB, strSQL, NULL, NULL, &strErr) != SQLITE_OK) {
        CPrintF("[RocketJumpTracker] SQL error: %s\n", strErr ? strErr : "unknown");
        sqlite3_free(strErr);
    }
}

// Commits one finished flight in a single transaction and fills the out-params.
static void RJT_CommitFlight(INDEX iPlayer, SFlightState& flight, DOUBLE tmLand,
    FLOAT fLandX, FLOAT fLandY, FLOAT fLandZ, FLOAT fLandHeight, const char* strLandSurface,
    INDEX* pbScored, INDEX* pbShouldAnnounce,
    FLOAT* pfAirtime, FLOAT* pfApexHeight, FLOAT* pfNetHeightDiff,
    FLOAT* pfPeakVerticalVelocity, FLOAT* pfAngleHitErrorDeg,
    INDEX* pctExplosions, INDEX* pctCannonballs, FLOAT* pfHPSpent) {

    FLOAT fNetHeightDiff = fLandHeight - flight.fLaunchSurfaceHeight;

    // Pass 1: angle hit error - degrees between the FIRST explosion kick's direction
    // and straight up. -1 = no explosion kick before the first jump-key kick (N/A).
    // Deliberately stops at the very first non-jump kick, per "we need just the first
    // one" - a second rocket/cannonball landing afterward doesn't change this figure.
    FLOAT fAngleHitErrorDeg = -1.0f;
    if (flight.aKicks.size() > 0 && !flight.aKicks[0].bIsJumpKick) {
        FLOAT fCos = flight.aKicks[0].fAngleCos;
        if (fCos > 1.0f) fCos = 1.0f;
        if (fCos < -1.0f) fCos = -1.0f;
        fAngleHitErrorDeg = (FLOAT)(acos(fCos) * (180.0 / 3.14159265358979323846));
    }

    // Pass 2: explosion count and cannonball count kept SEPARATE (cannonball sub-blasts
    // still grouped into one per cannonball), plus HP spent - over the WHOLE flight,
    // unlike pass 1, this covers every kick, not just the first.
    FLOAT fHPSpent = 0;
    INDEX ctExplosions = 0;   // non-cannonball explosions (rockets, impacts, etc.)
    INDEX ctCannonballs = 0;  // cannonball explosions, 4 (Iron) / 13 (Nuke) sub-blasts grouped into 1
    DOUBLE tmLastCannonballTick = -1000.0;
    for (size_t n = 0; n < flight.aKicks.size(); n++) {
        if (flight.aKicks[n].bIsJumpKick) continue;
        fHPSpent += flight.aKicks[n].fDamageAmount;

        if (flight.aKicks[n].iDamageType == DMT_CANNONBALL_EXPLOSION) {
            // Iron cannonballs fire 4 near-simultaneous sub-blasts, Nuke fires 13 - count
            // them as ONE cannonball explosion instead of 4/13 separate ones. They land
            // within a tick or so of each other; anything further apart is a different
            // cannonball entirely.
            if (flight.aKicks[n].tick - tmLastCannonballTick > 1.0) {
                ctCannonballs++;
            }
            tmLastCannonballTick = flight.aKicks[n].tick;
        }
        else {
            ctExplosions++;
        }
    }

    // Explosions required to score at all - a plain hill-climb with zero kicks was
    // still occasionally passing the height/HP gates on its own; this closes that off
    // directly rather than relying on those gates alone.
    INDEX bScored = (fHPSpent < rjt_fMaxHPSpent && fNetHeightDiff > rjt_fMinHeightDiff
        && (ctExplosions + ctCannonballs) > 0) ? 1 : 0;

    if (_pRJT_DB != NULL) {
        RJT_ExecOrWarn("BEGIN IMMEDIATE;");

        sqlite3_stmt* pStmtJump = NULL;
        sqlite3_prepare_v2(_pRJT_DB,
            "INSERT INTO jumps (player_id, player_name, launch_tick, launch_x, launch_y, launch_z, "
            "launch_surface, launch_surface_height, land_tick, land_x, land_y, land_z, land_surface, "
            "airtime, apex_height, net_height_diff, peak_vertical_velocity, angle_hit_error, "
            "explosion_count, cannonball_count, hp_spent, scored) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", -1, &pStmtJump, NULL);

        int iCol = 1;
        sqlite3_bind_int64(pStmtJump, iCol++, iPlayer);
        sqlite3_bind_text(pStmtJump, iCol++, flight.strPlayerName, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(pStmtJump, iCol++, flight.tmLaunch);
        sqlite3_bind_double(pStmtJump, iCol++, flight.fLaunchX);
        sqlite3_bind_double(pStmtJump, iCol++, flight.fLaunchY);
        sqlite3_bind_double(pStmtJump, iCol++, flight.fLaunchZ);
        sqlite3_bind_text(pStmtJump, iCol++, flight.strLaunchSurface, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(pStmtJump, iCol++, flight.fLaunchSurfaceHeight);
        sqlite3_bind_double(pStmtJump, iCol++, tmLand);
        sqlite3_bind_double(pStmtJump, iCol++, fLandX);
        sqlite3_bind_double(pStmtJump, iCol++, fLandY);
        sqlite3_bind_double(pStmtJump, iCol++, fLandZ);
        sqlite3_bind_text(pStmtJump, iCol++, strLandSurface, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(pStmtJump, iCol++, tmLand - flight.tmLaunch);
        sqlite3_bind_double(pStmtJump, iCol++, flight.fApexHeightGained);
        sqlite3_bind_double(pStmtJump, iCol++, fNetHeightDiff);
        sqlite3_bind_double(pStmtJump, iCol++, flight.fPeakVerticalVelocity);
        sqlite3_bind_double(pStmtJump, iCol++, fAngleHitErrorDeg);
        sqlite3_bind_int(pStmtJump, iCol++, (int)ctExplosions);
        sqlite3_bind_int(pStmtJump, iCol++, (int)ctCannonballs);
        sqlite3_bind_double(pStmtJump, iCol++, fHPSpent);
        sqlite3_bind_int(pStmtJump, iCol++, (int)bScored);

        sqlite3_step(pStmtJump);
        sqlite3_finalize(pStmtJump);

        sqlite3_int64 llJumpID = sqlite3_last_insert_rowid(_pRJT_DB);

        for (size_t k = 0; k < flight.aKicks.size(); k++) {
            const SKickRecord& kick = flight.aKicks[k];
            sqlite3_stmt* pStmtKick = NULL;
            sqlite3_prepare_v2(_pRJT_DB,
                "INSERT INTO kicks (jump_id, tick, is_jump_kick, damage_type, damage_amount, kick_damage, "
                "dir_x, dir_y, dir_z, vel_before_x, vel_before_y, vel_before_z, "
                "vel_after_x, vel_after_y, vel_after_z, vertical_delta_v) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);", -1, &pStmtKick, NULL);

            int j = 1;
            sqlite3_bind_int64(pStmtKick, j++, llJumpID);
            sqlite3_bind_double(pStmtKick, j++, kick.tick);
            sqlite3_bind_int(pStmtKick, j++, kick.bIsJumpKick);
            sqlite3_bind_int(pStmtKick, j++, kick.iDamageType);
            sqlite3_bind_double(pStmtKick, j++, kick.fDamageAmount);
            sqlite3_bind_double(pStmtKick, j++, kick.fKickDamage);
            sqlite3_bind_double(pStmtKick, j++, kick.fDirX);
            sqlite3_bind_double(pStmtKick, j++, kick.fDirY);
            sqlite3_bind_double(pStmtKick, j++, kick.fDirZ);
            sqlite3_bind_double(pStmtKick, j++, kick.fVelBeforeX);
            sqlite3_bind_double(pStmtKick, j++, kick.fVelBeforeY);
            sqlite3_bind_double(pStmtKick, j++, kick.fVelBeforeZ);
            sqlite3_bind_double(pStmtKick, j++, kick.fVelAfterX);
            sqlite3_bind_double(pStmtKick, j++, kick.fVelAfterY);
            sqlite3_bind_double(pStmtKick, j++, kick.fVelAfterZ);
            sqlite3_bind_double(pStmtKick, j++, kick.fVerticalDeltaV);

            sqlite3_step(pStmtKick);
            sqlite3_finalize(pStmtKick);
        }

        RJT_ExecOrWarn("COMMIT;");
    }

    if (pfAirtime) *pfAirtime = (FLOAT)(tmLand - flight.tmLaunch);
    if (pfApexHeight) *pfApexHeight = flight.fApexHeightGained;
    if (pfNetHeightDiff) *pfNetHeightDiff = fNetHeightDiff;
    if (pfPeakVerticalVelocity) *pfPeakVerticalVelocity = flight.fPeakVerticalVelocity;
    if (pfAngleHitErrorDeg) *pfAngleHitErrorDeg = fAngleHitErrorDeg;
    if (pctExplosions) *pctExplosions = ctExplosions;
    if (pctCannonballs) *pctCannonballs = ctCannonballs;
    if (pfHPSpent) *pfHPSpent = fHPSpent;
    if (pbScored) *pbScored = bScored;
    if (pbShouldAnnounce) *pbShouldAnnounce = (bScored && rjt_bAnnounceInChat) ? 1 : 0;
}

// ─── public API ─────────────────────────────────────────────────────────────

void RJT_Init(void) {
    if (sqlite3_open_v2(rjt_strDBPath, &_pRJT_DB,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        CPrintF("[RocketJumpTracker] Failed to open DB '%s': %s\n",
            (const char*)rjt_strDBPath, _pRJT_DB ? sqlite3_errmsg(_pRJT_DB) : "unknown");
        _pRJT_DB = NULL;
        return;
    }

    RJT_ExecOrWarn("PRAGMA journal_mode=WAL;");
    RJT_ExecOrWarn("PRAGMA synchronous=NORMAL;");
    RJT_ExecOrWarn(_strRJT_Schema);

    CPrintF("[RocketJumpTracker] Initialized DB at '%s'\n", (const char*)rjt_strDBPath);
}

void RJT_Shutdown(void) {
    for (std::map<INDEX, SFlightState>::iterator it = _mapRJT_ActiveFlights.begin();
        it != _mapRJT_ActiveFlights.end(); ++it) {
        if (it->second.bActive) {
            RJT_CommitFlight(it->first, it->second, it->second.tmLaunch,
                it->second.fLaunchX, it->second.fLaunchY, it->second.fLaunchZ,
                it->second.fLaunchSurfaceHeight, "shutdown",
                NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        }
    }
    _mapRJT_ActiveFlights.clear();

    if (_pRJT_DB != NULL) {
        sqlite3_close(_pRJT_DB);
        _pRJT_DB = NULL;
    }
}

void RJT_BeginFlight(INDEX iPlayer, const char* strPlayerName,
    float fLaunchX, float fLaunchY, float fLaunchZ, double tmLaunch,
    const char* strLaunchSurface, float fLaunchSurfaceHeight, float fHealthBeforeLaunch) {
    // fHealthBeforeLaunch is no longer used (the full-HP carry-forward gate was
    // removed - see the comment below) but stays in the signature so Player.es's
    // existing call site doesn't need touching, avoiding yet another parameter-count
    // build error for a change that's purely internal to this function.
    if (!rjt_bEnable) return;

    SFlightState& flight = _mapRJT_ActiveFlights[iPlayer];

    // If a flight was already open (missed landing event, or re-launched mid-air by
    // another explosion), commit it first instead of silently overwriting it.
    if (flight.bActive) {
        RJT_CommitFlight(iPlayer, flight, tmLaunch, fLaunchX, fLaunchY, fLaunchZ,
            fLaunchSurfaceHeight, "interrupted", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    }

    // Keep any kicks already logged for this player (RJT_LogKick buffers even with no
    // active flight) - the "shoot rocket while still grounded, then press jump late"
    // case: the explosion's kick lands BEFORE this liftoff edge fires. Only trust them
    // as part of THIS jump if they're recent (within 1 second) - an unrelated older
    // explosion from actual prior combat would already be far outside that window by
    // the time of a new, unrelated jump, so the time check alone covers what an
    // earlier "must be at full HP first" requirement was also trying to catch. That
    // HP requirement got removed: it was silently blocking this whole mechanism (and
    // everything built on it - peak velocity/height carry-forward, the position-at-kick
    // fix) any time the player wasn't at ~100 HP before jumping, which is the common
    // case for repeated testing or normal play without healing back up between jumps.
    std::vector<SKickRecord> aRJT_RecentKicks;
    FLOAT fRJT_CarriedPeakVel = 0;
    FLOAT fRJT_CarriedApexHeight = 0;
    BOOL bRJT_UseFirstKickPosition = FALSE;
    FLOAT fRJT_FirstKickX = 0, fRJT_FirstKickY = 0, fRJT_FirstKickZ = 0, fRJT_FirstKickHeight = 0;
    for (size_t m = 0; m < flight.aKicks.size(); m++) {
        if (tmLaunch - flight.aKicks[m].tick <= 1.0) aRJT_RecentKicks.push_back(flight.aKicks[m]);
    }
    // Only carry forward the peak/apex/position readings if we're ALSO carrying
    // forward the kicks that produced them - otherwise stale data could leak into
    // an unrelated fresh flight even after its kicks got filtered out above.
    if (aRJT_RecentKicks.size() > 0) {
        fRJT_CarriedPeakVel = flight.fPeakVerticalVelocity;
        fRJT_CarriedApexHeight = flight.fApexHeightGained;

        // Prefer the position captured synchronously at the first kick over whatever
        // Player.es's own last-known-position tracking passed in - en_penReference
        // staying non-NULL between the kick and this liftoff doesn't mean position
        // stayed frozen the whole time (see the header comment on RJT_LogKick).
        if (flight.bHasFirstKickPosition) {
            bRJT_UseFirstKickPosition = TRUE;
            fRJT_FirstKickX = flight.fFirstKickX;
            fRJT_FirstKickY = flight.fFirstKickY;
            fRJT_FirstKickZ = flight.fFirstKickZ;
            fRJT_FirstKickHeight = flight.fFirstKickHeight;
        }
    }

    flight = SFlightState();
    flight.bActive = TRUE;
    flight.strPlayerName = strPlayerName;
    flight.tmLaunch = tmLaunch;
    if (bRJT_UseFirstKickPosition) {
        flight.fLaunchX = fRJT_FirstKickX; flight.fLaunchY = fRJT_FirstKickY; flight.fLaunchZ = fRJT_FirstKickZ;
        flight.fLaunchSurfaceHeight = fRJT_FirstKickHeight;
    }
    else {
        flight.fLaunchX = fLaunchX; flight.fLaunchY = fLaunchY; flight.fLaunchZ = fLaunchZ;
        flight.fLaunchSurfaceHeight = fLaunchSurfaceHeight;
    }
    flight.strLaunchSurface = strLaunchSurface;
    flight.fPeakVerticalVelocity = fRJT_CarriedPeakVel;
    flight.fApexHeightGained = fRJT_CarriedApexHeight;
    flight.aKicks = aRJT_RecentKicks;
}

void RJT_UpdateFlightPeak(INDEX iPlayer, float fVerticalVelocity, float fHeightGained) {
    if (!rjt_bEnable) return;

    std::map<INDEX, SFlightState>::iterator it = _mapRJT_ActiveFlights.find(iPlayer);
    if (it == _mapRJT_ActiveFlights.end() || !it->second.bActive) return;

    SFlightState& flight = it->second;
    if (fHeightGained > flight.fApexHeightGained) flight.fApexHeightGained = fHeightGained;

    // Ascending only - a fast downward fall right before impact isn't "peak speed".
    if (fVerticalVelocity > flight.fPeakVerticalVelocity) {
        flight.fPeakVerticalVelocity = fVerticalVelocity;
    }
}

void RJT_AbortFlight(INDEX iPlayer) {
    // No rjt_bEnable guard - if a flight is open we want to be able to discard it
    // even if the tracker was disabled mid-flight.
    _mapRJT_ActiveFlights.erase(iPlayer);
}

void RJT_LogKick(INDEX iPlayer, double tmTick, INDEX iDamageType, INDEX bIsJumpKick,
    float fDamageAmount, float fKickDamage,
    float fDirX, float fDirY, float fDirZ,
    float fVelBeforeX, float fVelBeforeY, float fVelBeforeZ,
    float fVelAfterX, float fVelAfterY, float fVelAfterZ,
    float fVerticalDeltaV, float fAngleCos, float fVerticalVelNow, float fHeightNow,
    float fPosX, float fPosY, float fPosZ, float fHeightAtKick) {
    if (!rjt_bEnable) return;

    // Buffer even if there's no active flight yet - a kick fires before BeginFlight()
    // sees the resulting liftoff on the next tick.
    SFlightState& flight = _mapRJT_ActiveFlights[iPlayer];

    SKickRecord kick;
    kick.tick = tmTick;
    kick.iDamageType = iDamageType;
    kick.bIsJumpKick = bIsJumpKick;
    kick.fDamageAmount = fDamageAmount;
    kick.fKickDamage = fKickDamage;
    kick.fDirX = fDirX; kick.fDirY = fDirY; kick.fDirZ = fDirZ;
    kick.fVelBeforeX = fVelBeforeX; kick.fVelBeforeY = fVelBeforeY; kick.fVelBeforeZ = fVelBeforeZ;
    kick.fVelAfterX = fVelAfterX; kick.fVelAfterY = fVelAfterY; kick.fVelAfterZ = fVelAfterZ;
    kick.fVerticalDeltaV = fVerticalDeltaV;
    kick.fAngleCos = fAngleCos;

    flight.aKicks.push_back(kick);

    // First non-jump kick this buffering session - remember its position as the
    // candidate TRUE launch baseline. Never overwritten by later kicks in the same
    // session (en_penReference staying non-NULL doesn't mean position is frozen, so a
    // second/third kick's position could already be contaminated the same way).
    if (!bIsJumpKick && !flight.bHasFirstKickPosition) {
        flight.bHasFirstKickPosition = TRUE;
        flight.fFirstKickX = fPosX; flight.fFirstKickY = fPosY; flight.fFirstKickZ = fPosZ;
        flight.fFirstKickHeight = fHeightAtKick;
    }

    // Update peak trackers directly from this kick's own capture - fixes the ~1 tick
    // (your server's ~1.5 m/s) gravity-decay undercounting the later per-tick hook
    // would otherwise introduce, and works even pre-liftoff (no active flight yet),
    // since RJT_BeginFlight() carries these forward alongside the kicks themselves.
    if (fHeightNow > flight.fApexHeightGained) flight.fApexHeightGained = fHeightNow;
    if (fVerticalVelNow > flight.fPeakVerticalVelocity) flight.fPeakVerticalVelocity = fVerticalVelNow;
}

void RJT_EndFlight(INDEX iPlayer, double tmLand,
    float fLandX, float fLandY, float fLandZ, float fLandHeight, const char* strLandSurface,
    INDEX* pbValid, INDEX* pbScored, INDEX* pbShouldAnnounce,
    float* pfAirtime, float* pfApexHeight, float* pfNetHeightDiff, float* pfPeakVerticalVelocity,
    float* pfAngleHitErrorDeg, INDEX* pctExplosions, INDEX* pctCannonballs, float* pfHPSpent) {

    if (pbValid) *pbValid = 0;
    if (!rjt_bEnable) return;

    std::map<INDEX, SFlightState>::iterator it = _mapRJT_ActiveFlights.find(iPlayer);
    if (it == _mapRJT_ActiveFlights.end() || !it->second.bActive) return;

    RJT_CommitFlight(iPlayer, it->second, tmLand, fLandX, fLandY, fLandZ, fLandHeight, strLandSurface,
        pbScored, pbShouldAnnounce, pfAirtime, pfApexHeight, pfNetHeightDiff,
        pfPeakVerticalVelocity, pfAngleHitErrorDeg, pctExplosions, pctCannonballs, pfHPSpent);
    if (pbValid) *pbValid = 1;

    _mapRJT_ActiveFlights.erase(it);
}

void RJT_ExpireStaleFlights(double tmNow, double fMaxAirtime) {
    if (!rjt_bEnable) return;

    std::vector<INDEX> aStale;
    for (std::map<INDEX, SFlightState>::iterator it = _mapRJT_ActiveFlights.begin();
        it != _mapRJT_ActiveFlights.end(); ++it) {
        if (it->second.bActive && (tmNow - it->second.tmLaunch) > fMaxAirtime) aStale.push_back(it->first);
    }
    for (size_t i = 0; i < aStale.size(); i++) {
        // Abandoned (disconnect/crash) rather than a clean landing - discard, don't commit
        // a fabricated landing position for it.
        _mapRJT_ActiveFlights.erase(aStale[i]);
    }
}

float RJT_GetTeleportThreshold(void) {
    return rjt_fTeleportThreshold;
}

void RJT_AnnounceJump(const char* strMessageEN, const char* strMessageRU) {
    if (!_pNetwork->IsServer()) return;

    // PlayerDB keys language by PLAYER index (plb_Index, same as PlayerDB_OnJoin's
    // iNewPlayer), which is NOT the same as the client/session index used for
    // srv_assoSessions[]/SendChatToClient - a client can have multiple local players.
    // Looping over srv_assoSessions and reusing that same index for both was the bug:
    // it happened to look up the wrong player's language for a given client's message.
    CServer& srv = _pNetwork->ga_srvServer;
    FOREACHINSTATICARRAY(srv.srv_aplbPlayers, CPlayerBuffer, itplb) {
        if (!itplb->IsActive()) continue;

        INDEX iPlayer = itplb->plb_Index;    // PlayerDB's indexing convention
        if (PlayerDB_GetAnnounceOptOut(iPlayer)) continue;
        INDEX iClient = itplb->plb_iClient;  // what SendChatToClient actually wants

        BOOL bRU = (strcmp(PlayerDB_GetLanguage(iPlayer), "ru") == 0);
        // Non-empty sender - an empty CTString("") is a plausible thing for the client's
        // chat renderer (and the server console's own echo) to treat as invalid and
        // silently drop, which matches "notification sound plays but no text appears
        // anywhere, including the server console." A color-code-only string is non-empty
        // at the character level but renders as no visible name prefix.
        INetwork::SendChatToClient(iClient, CTString("^cfcc444"), CTString(bRU ? strMessageRU : strMessageEN));
        INetwork::SendChatToClient(0, CTString("^cfcc444"), CTString(bRU ? strMessageRU : strMessageEN));
    }
}

void NotifyCoopMarkerActivated(const char* strMessageEN, const char* strMessageRU) {
  if (!_pNetwork->IsServer()) return;

  static FLOAT _fLastAnnounce = -1000.0f;
  const FLOAT ANNOUNCE_COOLDOWN = 5.0f;

  FLOAT fNow = _pTimer->CurrentTick();
  if (fNow < _fLastAnnounce || fNow - _fLastAnnounce >= ANNOUNCE_COOLDOWN) {
      _fLastAnnounce = fNow;
  }
  else {
      return;
  }

  CServer &srv = _pNetwork->ga_srvServer;

  FOREACHINSTATICARRAY(srv.srv_aplbPlayers, CPlayerBuffer, itplb) {
    if (!itplb->IsActive()) continue;

    INDEX iPlayer = itplb->plb_Index;
    if (PlayerDB_GetAnnounceOptOut(iPlayer)) continue;
    INDEX iClient = itplb->plb_iClient;

    BOOL bRU = (strcmp(PlayerDB_GetLanguage(iPlayer), "ru") == 0);

    INetwork::SendChatToClient(0, CTString("^ceeee"), CTString(bRU ? strMessageRU : strMessageEN));
    INetwork::SendChatToClient(iClient, CTString("^ceeee"), CTString(bRU ? strMessageRU : strMessageEN));
  }
};


// ── Out-of-bounds detection ──
const FLOAT OOB_FALLTIME_THRESHOLD = 15.0f;  // tune based on your own testing

struct StuckTrackedPlayer { CEntity* penPlayer; BOOL bNotified; };
static StuckTrackedPlayer _aStuckState[16];  // match your real max player count
static INDEX _ctStuckTracked = 0;

static StuckTrackedPlayer* FindOrAddStuckEntry(CEntity* penPlayer) {
    for (INDEX i = 0; i < _ctStuckTracked; i++) {
        if (_aStuckState[i].penPlayer == penPlayer) return &_aStuckState[i];
    }
    if (_ctStuckTracked < ARRAYCOUNT(_aStuckState)) {
        StuckTrackedPlayer& entry = _aStuckState[_ctStuckTracked++];
        entry.penPlayer = penPlayer;
        entry.bNotified = FALSE;
        return &entry;
    }
    return NULL;
}

INDEX ent_bKickOnOutOfBounds = FALSE;  // NEEDS REGISTRATION - see open items below

void ReportStuckState(CEntity* penPlayer, BOOL bCurrentlyStuck, const char* strReasonEN, const char* strReasonRU) {
    if (!_pNetwork->IsServer()) return;

    StuckTrackedPlayer* pEntry = FindOrAddStuckEntry(penPlayer);
    if (pEntry == NULL) return;

    if (!bCurrentlyStuck) { pEntry->bNotified = FALSE; return; }
    if (pEntry->bNotified) return;
    pEntry->bNotified = TRUE;

    CTString strName = ((CPlayerEntity*)penPlayer)->GetPlayerName();
    CTString strMsgEN, strMsgRU;
    strMsgEN.PrintF("%s %s", strName, strReasonEN);
    strMsgRU.PrintF("%s %s", strName, strReasonRU);

    CServer& srv = _pNetwork->ga_srvServer;
    INDEX iOffendingClient = -1;

    FOREACHINSTATICARRAY(srv.srv_aplbPlayers, CPlayerBuffer, itplb) {
        if (!itplb->IsActive()) continue;
        INDEX iPlayer = itplb->plb_Index;
        INDEX iClient = itplb->plb_iClient;

        //if (itplb->plb_penPlayerEntity == penPlayer) { iOffendingClient = iClient; }  // VERIFY member name

        BOOL bRU = (strcmp(PlayerDB_GetLanguage(iPlayer), "ru") == 0);
        INetwork::SendChatToClient(0, CTString("^ceeee"), CTString(bRU ? strMsgRU : strMsgEN));
        INetwork::SendChatToClient(iClient, CTString("^ceeee"), CTString(bRU ? strMsgRU : strMsgEN));
    }

    if (ent_bKickOnOutOfBounds && iOffendingClient >= 0) {
        //INetwork::KickClient(iOffendingClient, CTString("Kicked for out-of-bounds exploit"));  // VERIFY this call exists
    }
};

void CheckFallTime(CEntity* penPlayer, FLOAT fFallTime) {
    ReportStuckState(penPlayer, fFallTime >= OOB_FALLTIME_THRESHOLD,
        "appears to be out of bounds", "похоже, находится за пределами карты");
};