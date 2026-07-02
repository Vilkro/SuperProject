/* GeoIP.h  -  Async IP geolocation + game ping   // 1111
 *
 * Drop GeoIP.cpp into Core/Query/ alongside PlayerDB.cpp.
 * Add Core\Query\GeoIP.cpp to ClCompile in Core.vcxproj.
 * No additional .lib files required.
 *
 * Shell symbols registered:
 *   void  GetClientLocation(INDEX iClient)
 *         Writes "City, Country" into cmd_strGeoResult.
 *         While the async lookup is still running: writes "IP (...)" so the
 *         script can tell the user the result is pending.
 *         Writes "" if the slot is inactive / no IP available.
 *
 *   CTString cmd_strGeoResult
 *         Set by GetClientLocation; read by the @locate script.
 *
 *   void  GetClientPing(INDEX iClient)
 *         Reads CPlayerBuffer::plb_iPing for the given client slot.
 *         Writes the value (ms) into cmd_fPingResult, or -1 if the
 *         slot has no active player.
 *
 *   FLOAT cmd_fPingResult
 *         Set by GetClientPing; read by the @ping script immediately after.
 *
 *   CTString GetClientName(INDEX iClient)
 *         Returns the player name for the given client slot, or "" if
 *         the slot has no active player.  Used by @locate to show
 *         "Name - City, Country" instead of "[slot] City, Country".
 */

#ifndef GEOIP_H
#define GEOIP_H

#ifdef PRAGMA_ONCE
#pragma once
#endif

void GeoIP_Init(void);
void GeoIP_Shutdown(void);
void GeoIP_Lookup(const char* szIP);
CTString GeoIP_GetCached(const char* szIP);

// Called from the main game thread (PlayerDB_ProcessCommands) every second.   // 1111
// Fires any join announcements whose async lookup has now completed.
void GeoIP_FirePendingAnnounces(void);                                         // 1111

// Cancel any pending announcement for this player IP.                          // 1111
// Call from PlayerDB_OnDisconnect before clearing the session slot.
void GeoIP_ClearPendingByIP(const char* szIP);                                 // 1111

void GeoIP_GetCachedSplit(const char* szIP, CTString& strCity, CTString& strCountry);  // 1111

extern FLOAT   cmd_fPingResult;
extern FLOAT   cmd_fNetPingResult;
extern CTString cmd_strGeoResult;
extern CTString cmd_strPlayerCoords;
extern CTString cmd_strCountryResult;   // 1111

#endif /* GEOIP_H */