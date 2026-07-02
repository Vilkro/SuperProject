/* Copyright (c) 2022-2026 Dreamy Cecil
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation

─── SQLite setup ────────────────────────────────────────────────────────────────
Requires sqlite3.h + sqlite3.c (amalgamation) from https://www.sqlite.org/download.html
Use version 3.7.17 for MSVC6 compatibility:
  https://www.sqlite.org/2013/sqlite-amalgamation-3071700.zip

1. Drop sqlite3.h and sqlite3.c into Core/Query/ (same folder as this file)
2. Add sqlite3.c to the ClassicsCore project in Visual Studio
3. In sqlite3.c file properties: C/C++ → Advanced → Compile As → C Code (/TC)
No extra defines needed for 3.7.17 on Win32.
─────────────────────────────────────────────────────────────────────────────── */

#include "StdH.h"
#include "Query/PlayerDB.h"
#include "Query/GeoIP.h"     // 1111
#include "Query/sqlite3.h"
#include "Networking/Modules/ClientLogging.h"  // 1111: _aClientIdentities, CClientRestriction

#include <time.h>
#include <string.h>

// ─── state ────────────────────────────────────────────────────────────────────

static sqlite3* _db = NULL;

struct SActiveSession {
    BOOL  bActive;
    char  szGUID[33];    // 32 hex chars + null terminator
    char  szName[256];   // generous for colour-coded names
    char  szIP[16];      // dotted-decimal; used to cancel pending GeoIP announce  // 1111
    char  szLang[3];     // "en" or "ru" + null terminator  // 1111
    long  tJoinTime;     // unix timestamp
};

static SActiveSession _aSessions[PLAYERDB_MAX_PLAYERS];

// ─── helpers ─────────────────────────────────────────────────────────────────

void PlayerDB_FormatGUID(const UBYTE* pGUID, char* szOut) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        szOut[i * 2] = hex[(pGUID[i] >> 4) & 0x0F];
        szOut[i * 2 + 1] = hex[pGUID[i] & 0x0F];
    }
    szOut[32] = '\0';
}

static void ExecSQL(const char* szSQL) {
    char* szErr = NULL;
    if (sqlite3_exec(_db, szSQL, NULL, NULL, &szErr) != SQLITE_OK) {
        CPrintF("[PlayerDB] SQL error: %s\n", szErr ? szErr : "unknown");
        sqlite3_free(szErr);
    }
}

static sqlite3_stmt* Prepare(const char* szSQL) {
    sqlite3_stmt* pStmt = NULL;
    if (sqlite3_prepare_v2(_db, szSQL, -1, &pStmt, NULL) != SQLITE_OK) {
        CPrintF("[PlayerDB] Prepare failed: %s\n  -> %s\n", sqlite3_errmsg(_db), szSQL);
        return NULL;
    }
    return pStmt;
}

static void Run(sqlite3_stmt* pStmt) {
    if (pStmt == NULL) return;
    int rc = sqlite3_step(pStmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        CPrintF("[PlayerDB] Step error: %s\n", sqlite3_errmsg(_db));
    }
    sqlite3_finalize(pStmt);
}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void PlayerDB_Init(void) {
    memset(_aSessions, 0, sizeof(_aSessions));

    if (sqlite3_open("PlayerStats.db", &_db) != SQLITE_OK) {
        CPrintF("[PlayerDB] Cannot open PlayerStats.db: %s\n", sqlite3_errmsg(_db));
        sqlite3_close(_db);
        _db = NULL;
        return;
    }

    // WAL journal so website readers don't block server writes
    ExecSQL("PRAGMA journal_mode=WAL;");

    // ── players table ──────────────────────────────────────────────────────────
    // One persistent row per unique masked GUID.
    // 'sessions' and 'playtime' are running totals updated at each disconnect.
    // 'country' is always NULL for now; fill via website or future GeoIP step.
    ExecSQL(
        "CREATE TABLE IF NOT EXISTS players ("
        "  guid       TEXT    PRIMARY KEY,"  // 32-char hex of pc_aubGUID
        "  name       TEXT    NOT NULL,"     // last known nickname
        "  last_ip    TEXT,"                 // last known IP (dotted-decimal)
        "  country    TEXT,"                 // 2-letter code; NULL until looked up
        "  first_seen INTEGER NOT NULL,"     // unix timestamp, set once
        "  last_seen  INTEGER NOT NULL,"     // unix timestamp, updated every join
        "  sessions   INTEGER DEFAULT 0,"   // number of completed sessions
        "  playtime   INTEGER DEFAULT 0"    // total seconds of playtime
        ");"
    );

    // ── lang column migration (1111) ───────────────────────────────────────────
    // SQLite (3.7.17) has no "ALTER TABLE ... ADD COLUMN IF NOT EXISTS", so check
    // via PRAGMA table_info first to avoid erroring on every server start once applied.
    {
        BOOL bHasLangColumn = FALSE;
        sqlite3_stmt* pPragma = Prepare("PRAGMA table_info(players);");
        if (pPragma) {
            while (sqlite3_step(pPragma) == SQLITE_ROW) {
                const char* szColName = (const char*)sqlite3_column_text(pPragma, 1);  // column 1 = name
                if (szColName && strcmp(szColName, "lang") == 0) {
                    bHasLangColumn = TRUE;
                    break;
                }
            }
            sqlite3_finalize(pPragma);
        }
        if (!bHasLangColumn) {
            ExecSQL("ALTER TABLE players ADD COLUMN lang TEXT DEFAULT 'en';");
        }
    }

    // ── sessions table ─────────────────────────────────────────────────────────
    // One row per play session. Use this for per-map stats or historical queries.
    // Lifetime deathmatch totals are aggregated by the website via SUM(frags) etc.
    ExecSQL(
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id      INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  guid    TEXT    NOT NULL,"   // references players.guid
        "  name    TEXT    NOT NULL,"   // name during this session
        "  map     TEXT,"               // world file at disconnect time
        "  frags   INTEGER DEFAULT 0,"  // ps_iKills at disconnect
        "  deaths  INTEGER DEFAULT 0,"  // ps_iDeaths at disconnect
        "  score   INTEGER DEFAULT 0,"  // ps_iScore at disconnect
        "  started INTEGER NOT NULL,"
        "  ended   INTEGER NOT NULL"
        ");"
    );

    ExecSQL("CREATE INDEX IF NOT EXISTS idx_sessions_guid ON sessions(guid);");
    ExecSQL("CREATE INDEX IF NOT EXISTS idx_sessions_map  ON sessions(map);");

    // ── bans table (1111) ─────────────────────────────────────────────────────
    // Persistent ban records managed by the web admin panel.
    // Checked on every player connect attempt via PlayerDB_CheckBan().
    ExecSQL(
        "CREATE TABLE IF NOT EXISTS bans ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  guid       TEXT,"           // 32-char hex; NULL = IP-only ban
        "  ip         TEXT,"           // dotted decimal; NULL = GUID-only ban
        "  reason     TEXT,"           // shown to player on connect
        "  admin      TEXT,"           // set by web UI
        "  banned_at  INTEGER NOT NULL,"
        "  expires_at INTEGER,"        // NULL = permanent, else unix timestamp
        "  active     INTEGER DEFAULT 1"
        ");"
    );
    ExecSQL("CREATE INDEX IF NOT EXISTS idx_bans_guid ON bans(guid);");
    ExecSQL("CREATE INDEX IF NOT EXISTS idx_bans_ip   ON bans(ip);");

    // ── pending_cmds table (1111) ──────────────────────────────────────────────
    // Kick/mute/unmute orders from the web panel; processed within ~1 second
    // by PlayerDB_ProcessCommands() hooked into CoreTimerHandler::OnSecond().
    ExecSQL(
        "CREATE TABLE IF NOT EXISTS pending_cmds ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  cmd        TEXT    NOT NULL,"  // 'kick', 'mute', 'unmute'
        "  target     TEXT    NOT NULL,"  // GUID (32 hex) or IP string
        "  by_ip      INTEGER DEFAULT 0," // 0 = GUID, 1 = IP
        "  param      TEXT,"              // reason (kick) or seconds string (mute)
        "  created_at INTEGER NOT NULL,"
        "  done       INTEGER DEFAULT 0,"
        "  result     TEXT"
        ");"
    );

    CPutString("[PlayerDB] PlayerStats.db ready.\n");
}

void PlayerDB_Shutdown(void) {
    if (_db != NULL) {
        sqlite3_close(_db);
        _db = NULL;
        CPutString("[PlayerDB] Database closed.\n");
    }
}

// ─── join ─────────────────────────────────────────────────────────────────────

void PlayerDB_OnJoin(INDEX iPlayer, const UBYTE* pGUID,
    const char* strName, const char* strIP) {
    if (_db == NULL) return;
    if (iPlayer < 0 || iPlayer >= PLAYERDB_MAX_PLAYERS) return;

    // Format GUID
    char szGUID[33];
    PlayerDB_FormatGUID(pGUID, szGUID);

    long tNow = (long)time(NULL);

    // Track session in memory
    SActiveSession& sess = _aSessions[iPlayer];
    sess.bActive = TRUE;
    sess.tJoinTime = tNow;
    strncpy(sess.szGUID, szGUID, 32);  sess.szGUID[32] = '\0';
    strncpy(sess.szName, strName, 255);  sess.szName[255] = '\0';
    strncpy(sess.szIP, strIP, 15);   sess.szIP[15] = '\0';   // 1111

    // INSERT new player if GUID not seen before (preserves first_seen)
    sqlite3_stmt* pStmt = Prepare(
        "INSERT OR IGNORE INTO players"
        "  (guid, name, last_ip, first_seen, last_seen)"
        "  VALUES (?, ?, ?, ?, ?);");
    if (pStmt) {
        sqlite3_bind_text(pStmt, 1, szGUID, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 2, strName, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 3, strIP, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(pStmt, 4, (int)tNow);
        sqlite3_bind_int(pStmt, 5, (int)tNow);
        Run(pStmt);
    }

    // UPDATE mutable fields for returning players
    pStmt = Prepare(
        "UPDATE players SET name=?, last_ip=?, last_seen=? WHERE guid=?;");
    if (pStmt) {
        sqlite3_bind_text(pStmt, 1, strName, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 2, strIP, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(pStmt, 3, (int)tNow);
        sqlite3_bind_text(pStmt, 4, szGUID, -1, SQLITE_TRANSIENT);
        Run(pStmt);
    }

    // ── 1111: load stored language preference into the in-memory cache ─────────
    strncpy(sess.szLang, "en", 2);  sess.szLang[2] = '\0';  // default
    pStmt = Prepare("SELECT lang FROM players WHERE guid=?;");
    if (pStmt) {
        sqlite3_bind_text(pStmt, 1, szGUID, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(pStmt) == SQLITE_ROW) {
            const char* szStoredLang = (const char*)sqlite3_column_text(pStmt, 0);
            if (szStoredLang && strcmp(szStoredLang, "ru") == 0) {
                strncpy(sess.szLang, "ru", 2);  sess.szLang[2] = '\0';
            }
        }
        sqlite3_finalize(pStmt);
    }
}

// ─── disconnect ───────────────────────────────────────────────────────────────

void PlayerDB_OnDisconnect(INDEX iPlayer, const char* strMap,
    INDEX iFrags, INDEX iDeaths, INDEX iScore) {
    if (_db == NULL) return;
    if (iPlayer < 0 || iPlayer >= PLAYERDB_MAX_PLAYERS) return;

    SActiveSession& sess = _aSessions[iPlayer];
    if (!sess.bActive) return;

    long tNow = (long)time(NULL);
    long tPlayed = tNow - sess.tJoinTime;
    if (tPlayed < 0) tPlayed = 0;

    // Insert session row
    sqlite3_stmt* pStmt = Prepare(
        "INSERT INTO sessions (guid, name, map, frags, deaths, score, started, ended)"
        "  VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
    if (pStmt) {
        sqlite3_bind_text(pStmt, 1, sess.szGUID, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 2, sess.szName, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 3, strMap ? strMap : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(pStmt, 4, (int)iFrags);
        sqlite3_bind_int(pStmt, 5, (int)iDeaths);
        sqlite3_bind_int(pStmt, 6, (int)iScore);
        sqlite3_bind_int(pStmt, 7, (int)sess.tJoinTime);
        sqlite3_bind_int(pStmt, 8, (int)tNow);
        Run(pStmt);
    }

    // Update player aggregate stats
    pStmt = Prepare(
        "UPDATE players SET"
        "  sessions  = sessions + 1,"
        "  playtime  = playtime + ?,"
        "  last_seen = ?"
        "  WHERE guid = ?;");
    if (pStmt) {
        sqlite3_bind_int(pStmt, 1, (int)tPlayed);
        sqlite3_bind_int(pStmt, 2, (int)tNow);
        sqlite3_bind_text(pStmt, 3, sess.szGUID, -1, SQLITE_TRANSIENT);
        Run(pStmt);
    }

    GeoIP_ClearPendingByIP(sess.szIP);  // 1111
    memset(&sess, 0, sizeof(sess));
}

void PlayerDB_SetCountry(const char* szIP, const char* szCountry) {
    if (_db == NULL) return;
    sqlite3_stmt* pStmt = Prepare(
        "UPDATE players SET country=? WHERE last_ip=?;");
    if (pStmt) {
        sqlite3_bind_text(pStmt, 1, szCountry, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 2, szIP, -1, SQLITE_TRANSIENT);
        Run(pStmt);
    }
}

// ─────────────────────────────────────────────────────────────────── 1111 ────
// Language preference — fast in-memory read/write, persisted to players.lang.
// GetLanguage always returns "en" or "ru"; SetLanguage silently ignores
// anything other than those two values.
// ─────────────────────────────────────────────────────────────────────────────

const char* PlayerDB_GetLanguage(INDEX iPlayer) {    // 1111
    if (iPlayer < 0 || iPlayer >= PLAYERDB_MAX_PLAYERS) return "en";

    SActiveSession& sess = _aSessions[iPlayer];
    if (!sess.bActive) return "en";

    return (strcmp(sess.szLang, "ru") == 0) ? "ru" : "en";
}

void PlayerDB_SetLanguage(INDEX iPlayer, const char* szLang) {    // 1111
    if (iPlayer < 0 || iPlayer >= PLAYERDB_MAX_PLAYERS) return;
    if (szLang == NULL) return;
    if (strcmp(szLang, "en") != 0 && strcmp(szLang, "ru") != 0) return;

    SActiveSession& sess = _aSessions[iPlayer];
    if (!sess.bActive) return;

    strncpy(sess.szLang, szLang, 2);  sess.szLang[2] = '\0';

    if (_db == NULL) return;
    sqlite3_stmt* pStmt = Prepare("UPDATE players SET lang=? WHERE guid=?;");
    if (pStmt) {
        sqlite3_bind_text(pStmt, 1, szLang, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(pStmt, 2, sess.szGUID, -1, SQLITE_TRANSIENT);
        Run(pStmt);
    }
}

// ─────────────────────────────────────────────────────────────────── 1111 ────
// Ban check — called from MessageProcessing.cpp before accepting a player.
// Returns a non-empty disconnect reason if banned; empty string = clear.
// ─────────────────────────────────────────────────────────────────────────────

CTString PlayerDB_CheckBan(const char* szGUID, const char* szIP) {
    if (_db == NULL) return CTString("");

    sqlite3_stmt* pStmt = Prepare(
        "SELECT COALESCE(reason,'\n^cff1111YOU ARE BANNED FROM THIS SERVER^r')"
        " FROM bans"
        " WHERE active=1"
        "   AND (expires_at IS NULL OR expires_at > ?)"
        "   AND (guid=? OR ip=?)"
        " LIMIT 1;"
    );
    if (!pStmt) return CTString("");

    sqlite3_bind_int(pStmt, 1, (int)time(NULL));
    sqlite3_bind_text(pStmt, 2, szGUID ? szGUID : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(pStmt, 3, szIP ? szIP : "", -1, SQLITE_TRANSIENT);

    CTString strReason = "";
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
        const char* sz = (const char*)sqlite3_column_text(pStmt, 0);
        strReason = sz ? CTString(sz) : CTString("\n^cff1111YOU ARE BANNED FROM THIS SERVER^r");
    }
    sqlite3_finalize(pStmt);
    return strReason;
}

// ─────────────────────────────────────────────────────────────────── 1111 ────
// Command processor — called every game second from CoreTimerHandler::OnSecond.
// Reads up to 16 pending_cmds rows, finds the matching identity in
// _aClientIdentities by GUID or IP, executes the action, and marks done=1.
// ─────────────────────────────────────────────────────────────────────────────

struct SAdminCmd {           // 1111
    int  iID;
    char szCmd[16];            // 'kick', 'mute', 'unmute'
    char szTarget[64];         // 32-char GUID or dotted IP
    int  bByIP;                // 0 = GUID, 1 = IP
    char szParam[128];         // reason (kick) or seconds string (mute)
};

void PlayerDB_ProcessCommands(void) {
    GeoIP_FirePendingAnnounces();  // 1111 — runs even when DB has no pending cmds

    if (_db == NULL) return;

    // Collect a small batch, closing the SELECT before touching engine structures
    SAdminCmd aBatch[16];
    int nBatch = 0;

    sqlite3_stmt* pSel = Prepare(
        "SELECT id, cmd, target, by_ip, COALESCE(param,'')"
        " FROM pending_cmds WHERE done=0 ORDER BY id ASC LIMIT 16;"
    );
    if (!pSel) return;

    while (sqlite3_step(pSel) == SQLITE_ROW && nBatch < 16) {
        SAdminCmd& c = aBatch[nBatch++];
        c.iID = sqlite3_column_int(pSel, 0);
        c.bByIP = sqlite3_column_int(pSel, 3);

        const char* s1 = (const char*)sqlite3_column_text(pSel, 1);
        const char* s2 = (const char*)sqlite3_column_text(pSel, 2);
        const char* s4 = (const char*)sqlite3_column_text(pSel, 4);
        strncpy(c.szCmd, s1 ? s1 : "", 15);  c.szCmd[15] = '\0';
        strncpy(c.szTarget, s2 ? s2 : "", 63);  c.szTarget[63] = '\0';
        strncpy(c.szParam, s4 ? s4 : "", 127); c.szParam[127] = '\0';
    }
    sqlite3_finalize(pSel);

    if (nBatch == 0) return;

    for (int iCmd = 0; iCmd < nBatch; iCmd++) {
        SAdminCmd& c = aBatch[iCmd];
        CTString strResult = "";

        // ── find identity index by GUID or IP ──────────────────────────────────
        INDEX iIdentity = -1;
        const INDEX ctIdents = _aClientIdentities.Count();

        for (INDEX j = 0; j < ctIdents && iIdentity == -1; j++) {
            CClientIdentity& ci = _aClientIdentities[j];

            if (!c.bByIP) {
                // Match by GUID: format each stored character GUID and compare
                const INDEX ctChars = ci.aCharacters.Count();
                for (INDEX k = 0; k < ctChars; k++) {
                    char szFmt[33];
                    PlayerDB_FormatGUID(ci.aCharacters[k].pc_aubGUID, szFmt);
                    if (strcmp(szFmt, c.szTarget) == 0) { iIdentity = j; break; }
                }
            }
            else {
                // Match by IP: compare stored host strings
                const INDEX ctAddrs = ci.aAddresses.Count();
                for (INDEX k = 0; k < ctAddrs; k++) {
                    CTString strHost = ci.aAddresses[k].GetHost();
                    if (strcmp(strHost.str_String, c.szTarget) == 0) { iIdentity = j; break; }
                }
            }
        }

        // ── execute action ──────────────────────────────────────────────────────
        if (iIdentity == -1) {
            strResult.PrintF("not_found: %s", c.szTarget);

        }
        else if (strcmp(c.szCmd, "kick") == 0) {
            CTString strReason = (c.szParam[0] != '\0')
                ? CTString(c.szParam) : CTString("\n^cf8f644KICKED BY ADMIN^r");
            strResult = CClientRestriction::KickClient(iIdentity, strReason);

        }
        else if (strcmp(c.szCmd, "mute") == 0) {
            DOUBLE fSecs = (DOUBLE)atof(c.szParam[0] != '\0' ? c.szParam : "300");
            strResult = CClientRestriction::MuteClient(iIdentity, fSecs);

        }
        else if (strcmp(c.szCmd, "unmute") == 0) {
            _aClientIdentities[iIdentity].crRestrictions.SetMuteTime(0);
            strResult.PrintF("unmuted identity %d", (int)iIdentity);

        }
        else {
            strResult.PrintF("unknown_cmd: %s", c.szCmd);
        }

        // ── mark row done ───────────────────────────────────────────────────────
        sqlite3_stmt* pUpd = Prepare(
            "UPDATE pending_cmds SET done=1, result=? WHERE id=?;"
        );
        if (pUpd) {
            sqlite3_bind_text(pUpd, 1, strResult.str_String, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(pUpd, 2, c.iID);
            Run(pUpd);
        }
    }
}