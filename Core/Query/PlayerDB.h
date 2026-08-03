/* Copyright (c) 2022-2026 Dreamy Cecil
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation */     //   1111

#ifndef PLAYERDB_H
#define PLAYERDB_H

#ifdef PRAGMA_ONCE
#pragma once
#endif

// Maximum simultaneous player slots (SERVER_CLIENTS in SE1)
#define PLAYERDB_MAX_PLAYERS 16

// ─── lifecycle ────────────────────────────────────────────────────────────────
// Call from the end of InitQuery() in Core/Query/QueryManager.cpp
extern void PlayerDB_Init(void);

// Call from ClassicsPatch_Shutdown() in Core/Core.cpp
extern void PlayerDB_Shutdown(void);

// ─── events ───────────────────────────────────────────────────────────────────
// Call from IProcessPacket::OnPlayerConnectRequest() in Core/Networking/MessageProcessing.cpp
//   iPlayer  - player slot index (iNewPlayer)
//   pGUID    - pointer to pc_aubGUID (16 raw bytes from CPlayerCharacter)
//   strName  - pcCharacter.GetName()
//   strIP    - addr.GetIPAsString().str_String (from IClientLogging::GetAddress)
extern void PlayerDB_OnJoin(INDEX iPlayer, const UBYTE* pGUID,
    const char* strName, const char* strIP);

// Call from MSG_SEQ_REMPLAYER in Core/Patches/Network.cpp
//   iPlayer  - player slot index (iPlayer from nmMessage)
//   strMap   - _pNetwork->ga_World.wo_fnmFileName.str_String
//   iFrags   - penRemPlayer->m_psGameStats.ps_iKills
//   iDeaths  - penRemPlayer->m_psGameStats.ps_iDeaths
//   iScore   - penRemPlayer->m_psGameStats.ps_iScore
extern void PlayerDB_OnDisconnect(INDEX iPlayer, const char* strMap,
    INDEX iFrags, INDEX iDeaths, INDEX iScore);

extern void PlayerDB_SetCountry(const char* szIP, const char* szCountry);   // ip2country callback

// ── web admin command queue (1111) ────────────────────────────────────────────

// Called from MessageProcessing.cpp::OnPlayerConnectRequest.
// Returns a non-empty ban reason if the player should be rejected; empty = allow.
extern CTString PlayerDB_CheckBan(const char* szGUID, const char* szIP);  // 1111

// Called from CoreTimerHandler::OnSecond every game second.
// Reads pending_cmds rows, executes kick/mute/unmute via CClientRestriction,
// and marks each row done=1 with a result string.
extern void PlayerDB_ProcessCommands(void);  // 1111

// ─── utility ─────────────────────────────────────────────────────────────────
// Convert 16 raw GUID bytes to a 32-char upper-case hex string.
// szOut must be at least 33 bytes.
extern void PlayerDB_FormatGUID(const UBYTE* pGUID, char* szOut);

// ── language preference (1111) ─────────────────────────────────────────────────
// Fast in-memory read; always returns "en" or "ru", defaults to "en" for
// inactive/unknown slots. Populated by PlayerDB_OnJoin() from players.lang.
extern const char* PlayerDB_GetLanguage(INDEX iPlayer);  // 1111

// Updates the in-memory cache and persists to players.lang keyed by GUID.
// Silently ignores anything other than "en"/"ru".
extern void PlayerDB_SetLanguage(INDEX iPlayer, const char* szLang);  // 1111

// ── announcement preference (RJT) ───────────────────────────────────────────
// Fast in-memory read; FALSE (announcements ON) by default for inactive/unknown
// slots - same shape as PlayerDB_GetLanguage(). Populated by PlayerDB_OnJoin()
// from players.rjt_optout.
extern BOOL PlayerDB_GetAnnounceOptOut(INDEX iPlayer);   // TRUE = opted out

// Updates the in-memory cache and persists to players.rjt_optout, keyed by GUID -
// same mechanism PlayerDB_SetLanguage() already uses for players.lang.
extern void PlayerDB_SetAnnounceOptOut(INDEX iPlayer, BOOL bOptOut);

#endif // PLAYERDB_H