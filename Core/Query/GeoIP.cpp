/* GeoIP.cpp  -  Async geolocation + game ping   // 1111
 *
 * VC98 / Win32 compatible. No WinHTTP, no STL, no C++11.
 *
 * Uses:
 *   winsock2.h + ws2_32.lib  - raw TCP HTTP/1.0 GET (already linked by ClassicsPatch)
 *   windows.h                - CreateThread, CRITICAL_SECTION (via StdH.h)
 *   CPlayerBuffer::plb_iPing - engine-native ping, no extra libs needed
 *
 * To add to the Core project:
 *   Add Core\Query\GeoIP.cpp to ClCompile in Core.vcxproj (same as PlayerDB.cpp).
 *   No additional .lib files required.
 *
 * Geo API: http://ip-api.com/json/{ip}?fields=status,city,country
 *   Free, no key, 45 req/min.  HTTP only (no HTTPS needed for lookup data).
 *
 * Shell symbols:
 *   void GetClientLocation(INDEX iClient)
 *       Writes result into cmd_strGeoResult (global, readable from scripts).
 *       "City, Country" when resolved; "IP (...)" while pending; "" if inactive.
 *   void GetClientPing(INDEX iClient)
 *       Writes ms into cmd_fPingResult; -1 if unreachable.
 *   FLOAT cmd_fPingResult
 *       Set by GetClientPing; read by @ping script immediately after.
 *   CTString cmd_strGeoResult
 *       Set by GetClientLocation; read by @locate script immediately after.
 *   CTString GetClientName(INDEX iClient)
 *       Returns the player name for the slot, or "" if no active player.
 */

#include "StdH.h"

#include "Query/GeoIP.h"
#include "Query/PlayerDB.h"

#include "Networking/MessageProcessing.h"
#include "Networking/NetworkFunctions.h"
#include "Networking/Modules.h"

 /* No extra headers needed beyond StdH.h.
    windows.h, winsock2.h and all engine types come in via StdH.h -> Engine.h. */

    /* ---- Cache ------------------------------------------------------------ */

#define GEOCACHE_MAX  64   /* max unique IPs remembered */
#define GEOIP_PENDING "..."

struct SGeoEntry {
    char szIP[16];        /* dotted-decimal, null-terminated */
    char szLocation[64];  /* "City, Country" or GEOIP_PENDING or "" */
    BOOL bUsed;
};

static SGeoEntry     _aCache[GEOCACHE_MAX];
static CRITICAL_SECTION _csCache;
static BOOL _bCacheInit = FALSE;

CTString cmd_strCountryResult = "";   /* 1111 */

CTString cmd_strPlayerCoords = "";   /* "x,y,z" set by GetClientCoords */


/* -- Network ping (ICMP) --------------------------------------------- 1111 --
 *
 * Uses IcmpSendEcho loaded at runtime from iphlpapi.dll so no extra .lib
 * is required.  Each call spawns a small worker thread (same pattern as
 * GeoLookupThread).  The result is stored in a tiny IP-keyed cache and
 * copied into cmd_fNetPingResult once ready.
 * ----------------------------------------------------------------------- */

typedef DWORD ICMP_IPAddr;              /* 1111 */

typedef struct {                        /* 1111: mirrors IP_OPTION_INFORMATION */
    UCHAR  Ttl;
    UCHAR  Tos;
    UCHAR  Flags;
    UCHAR  OptionsSize;
    PUCHAR OptionsData;
} SIpOptionInfo;

typedef struct {                        /* 1111: mirrors ICMP_ECHO_REPLY */
    ICMP_IPAddr    Address;
    ULONG          Status;
    ULONG          RoundTripTime;   /* ms */
    USHORT         DataSize;
    USHORT         Reserved;
    PVOID          Data;
    SIpOptionInfo  Options;
} SIcmpEchoReply;

typedef HANDLE(WINAPI* FN_IcmpCreateFile)(void);                       /* 1111 */
typedef BOOL(WINAPI* FN_IcmpCloseHandle)(HANDLE);                   /* 1111 */
typedef DWORD(WINAPI* FN_IcmpSendEcho)(HANDLE, ICMP_IPAddr, LPVOID,  /* 1111 */
    WORD, PVOID, LPVOID, DWORD, DWORD);

#define PING_CACHE_MAX 16                                               /* 1111 */
#define PING_TIMEOUT_MS 3000                                            /* 1111 */
#define PING_PENDING -2.0f                                              /* 1111 */

struct SPingEntry {                                                     /* 1111 */
    char  szIP[16];
    FLOAT fRTT;
    BOOL  bUsed;
};

static SPingEntry       _aPingCache[PING_CACHE_MAX];                   /* 1111 */
static CRITICAL_SECTION _csPing;                                       /* 1111 */
static BOOL             _bPingInit = FALSE;                            /* 1111 */

struct SPingItem {                                                      /* 1111 */
    char szIP[16];
};

static DWORD WINAPI PingThread(LPVOID pArg) {                         /* 1111 */
    SPingItem* pItem = (SPingItem*)pArg;
    char szIP[16];
    strncpy(szIP, pItem->szIP, 15);
    szIP[15] = '\0';
    free(pItem);

    FLOAT fResult = -1.0f;

    ICMP_IPAddr dwDest = (ICMP_IPAddr)inet_addr(szIP);
    if (dwDest == INADDR_NONE) {
        goto store;
    }

    {
        HMODULE hLib = LoadLibraryA("iphlpapi.dll");
        if (!hLib) goto store;

        FN_IcmpCreateFile  fnCreate = (FN_IcmpCreateFile)GetProcAddress(hLib, "IcmpCreateFile");
        FN_IcmpCloseHandle fnClose = (FN_IcmpCloseHandle)GetProcAddress(hLib, "IcmpCloseHandle");
        FN_IcmpSendEcho    fnSend = (FN_IcmpSendEcho)GetProcAddress(hLib, "IcmpSendEcho");

        if (!fnCreate || !fnClose || !fnSend) {
            FreeLibrary(hLib);
            goto store;
        }

        HANDLE hIcmp = fnCreate();
        if (hIcmp == INVALID_HANDLE_VALUE) {
            FreeLibrary(hLib);
            goto store;
        }

        char aPayload[32];
        memset(aPayload, 'E', sizeof(aPayload));

        char aReply[sizeof(SIcmpEchoReply) + sizeof(aPayload) + 8];
        memset(aReply, 0, sizeof(aReply));

        DWORD dwReplied = fnSend(
            hIcmp,
            dwDest,
            aPayload, (WORD)sizeof(aPayload),
            NULL,
            aReply, (DWORD)sizeof(aReply),
            PING_TIMEOUT_MS);

        if (dwReplied > 0) {
            SIcmpEchoReply* pReply = (SIcmpEchoReply*)aReply;
            if (pReply->Status == 0) {
                fResult = (FLOAT)pReply->RoundTripTime;
            }
        }

        fnClose(hIcmp);
        FreeLibrary(hLib);
    }

store:
    if (_bPingInit) {
        EnterCriticalSection(&_csPing);
        for (int i = 0; i < PING_CACHE_MAX; i++) {
            if (_aPingCache[i].bUsed && strcmp(_aPingCache[i].szIP, szIP) == 0) {
                _aPingCache[i].fRTT = fResult;
                break;
            }
        }
        LeaveCriticalSection(&_csPing);
    }
    return 0;
}

static FLOAT PingLookup(const char* szIP) {                           /* 1111 */
    if (!_bPingInit || !szIP || !szIP[0]) return -1.0f;

    EnterCriticalSection(&_csPing);

    for (int i = 0; i < PING_CACHE_MAX; i++) {
        if (_aPingCache[i].bUsed && strcmp(_aPingCache[i].szIP, szIP) == 0) {
            FLOAT f = _aPingCache[i].fRTT;
            LeaveCriticalSection(&_csPing);
            return f;
        }
    }

    int iFree = -1;
    for (int k = 0; k < PING_CACHE_MAX; k++) {
        if (!_aPingCache[k].bUsed) { iFree = k; break; }
    }
    if (iFree == -1) {
        iFree = 0;
    }

    SPingEntry& e = _aPingCache[iFree];
    e.bUsed = TRUE;
    e.fRTT = PING_PENDING;
    strncpy(e.szIP, szIP, 15);
    e.szIP[15] = '\0';

    LeaveCriticalSection(&_csPing);

    SPingItem* pItem = (SPingItem*)malloc(sizeof(SPingItem));
    if (pItem) {
        strncpy(pItem->szIP, szIP, 15);
        pItem->szIP[15] = '\0';
        HANDLE hThread = CreateThread(NULL, 0, PingThread, pItem, 0, NULL);
        if (hThread) CloseHandle(hThread);
        else { free(pItem); }
    }
    return PING_PENDING;
}

/* ── Deferred join announcements ────────────────────────────────── 1111 ──
 *
 * When SayJoinLocationFunc fires before the async lookup has returned, it
 * registers a pending entry here instead of announcing "from ...".
 * GeoLookupThread calls _GeoAnnounce_Signal() once the result is cached.
 * GeoIP_FirePendingAnnounces() runs on the main game thread every second
 * (called from PlayerDB_ProcessCommands) and fires the actual chat message.
 * GeoIP_ClearPendingByIP() is called on disconnect to drop stale entries.
 * ─────────────────────────────────────────────────────────────────────── */

struct SGeoAnnounce {                               /* 1111 */
    BOOL  bPending;    /* slot is in use                          */
    BOOL  bReady;      /* lookup thread has stored the result     */
    char  szIP[16];    /* player IP — match key for thread signal */
    char  szName[256]; /* decorated name, for the chat message    */
    INDEX iExclude;    /* client slot excluded from the broadcast */
};

#define GEOANNOUNCE_MAX 16                           /* 1111 */
static SGeoAnnounce      _aPending[GEOANNOUNCE_MAX]; /* 1111 */
static CRITICAL_SECTION  _csPending;                 /* 1111 */
static BOOL              _bPendingInit = FALSE;       /* 1111 */

static void CacheInit(void) {
    if (_bCacheInit) return;
    InitializeCriticalSection(&_csCache);
    memset(_aCache, 0, sizeof(_aCache));
    _bCacheInit = TRUE;
}

/* Find entry by IP. Returns pointer into _aCache or NULL. Caller must hold _csCache. */
static SGeoEntry* CacheFind(const char* szIP) {
    for (int i = 0; i < GEOCACHE_MAX; i++) {
        if (_aCache[i].bUsed && strcmp(_aCache[i].szIP, szIP) == 0)
            return &_aCache[i];
    }
    return NULL;
}

/* Find or create entry. Returns NULL if cache is full. Caller must hold _csCache. */
static SGeoEntry* CacheGetOrCreate(const char* szIP) {
    SGeoEntry* p = CacheFind(szIP);
    if (p) return p;
    for (int i = 0; i < GEOCACHE_MAX; i++) {
        if (!_aCache[i].bUsed) {
            _aCache[i].bUsed = TRUE;
            strncpy(_aCache[i].szIP, szIP, 15);
            _aCache[i].szIP[15] = '\0';
            _aCache[i].szLocation[0] = '\0';
            return &_aCache[i];
        }
    }
    return NULL;  /* cache full */
}

/* ---- Minimal JSON field extractor ------------------------------------- */

/* Finds the value of "key":"value" in szJson.
   Writes up to nOut-1 chars into szOut.  Returns TRUE on success. */
static BOOL JsonGetString(const char* szJson,
    const char* szKey,
    char* szOut, int nOut) {
    char szPattern[64];
    _snprintf(szPattern, sizeof(szPattern) - 1, "\"%s\":\"", szKey);
    szPattern[sizeof(szPattern) - 1] = '\0';

    const char* p = strstr(szJson, szPattern);
    if (!p) return FALSE;

    p += strlen(szPattern);
    const char* e = strchr(p, '"');
    if (!e) return FALSE;

    int len = (int)(e - p);
    if (len >= nOut) len = nOut - 1;
    memcpy(szOut, p, len);
    szOut[len] = '\0';
    return TRUE;
}

/* ---- Raw TCP HTTP/1.0 GET ---------------------------------------------- */

static BOOL HttpGet(const char* szHost, const char* szPath,
    char* szBody, int nBody) {
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in sin;
    char szRequest[256];
    int  iSent, iTotal, iRecv;
    BOOL bOk = FALSE;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(80);
    sin.sin_addr.s_addr = inet_addr(szHost);

    if (sin.sin_addr.s_addr == INADDR_NONE) {
        struct hostent* phe = gethostbyname(szHost);
        if (!phe) return FALSE;
        sin.sin_addr = *(struct in_addr*)phe->h_addr;
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return FALSE;

    DWORD dwTimeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&dwTimeout, sizeof(dwTimeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&dwTimeout, sizeof(dwTimeout));

    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) == SOCKET_ERROR)
        goto cleanup;

    _snprintf(szRequest, sizeof(szRequest) - 1,
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        szPath, szHost);
    szRequest[sizeof(szRequest) - 1] = '\0';

    iSent = send(s, szRequest, strlen(szRequest), 0);
    if (iSent == SOCKET_ERROR) goto cleanup;

    iTotal = 0;
    while (iTotal < nBody - 1) {
        iRecv = recv(s, szBody + iTotal, nBody - 1 - iTotal, 0);
        if (iRecv <= 0) break;
        iTotal += iRecv;
    }
    szBody[iTotal] = '\0';

    {
        char* pBody = strstr(szBody, "\r\n\r\n");
        if (pBody) {
            pBody += 4;
            memmove(szBody, pBody, strlen(pBody) + 1);
            bOk = TRUE;
        }
    }

cleanup:
    closesocket(s);
    return bOk;
}

/* ---- Worker thread ----------------------------------------------------- */

struct SGeoWorkItem {
    char szIP[16];
};

static void _GeoAnnounce_Signal(const char* szIP);  /* 1111 — forward decl; defined below GeoLookupThread */

static DWORD WINAPI GeoLookupThread(LPVOID lpParam) {
    SGeoWorkItem* pItem = (SGeoWorkItem*)lpParam;
    char szIP[16];
    strncpy(szIP, pItem->szIP, 15);
    szIP[15] = '\0';
    delete pItem;

    char szPath[64];
    _snprintf(szPath, sizeof(szPath) - 1,
        "/json/%s?fields=status,city,country", szIP);
    szPath[sizeof(szPath) - 1] = '\0';

    char szBody[512];
    char szLocation[108];
    szLocation[0] = '\0';

    if (HttpGet("ip-api.com", szPath, szBody, sizeof(szBody))) {
        char szStatus[16], szCity[48], szCountry[48];
        szStatus[0] = szCity[0] = szCountry[0] = '\0';

        JsonGetString(szBody, "status", szStatus, sizeof(szStatus));
        JsonGetString(szBody, "city", szCity, sizeof(szCity));
        JsonGetString(szBody, "country", szCountry, sizeof(szCountry));

        if (strcmp(szStatus, "success") == 0) {
            if (szCity[0] && szCountry[0]) {
                _snprintf(szLocation, sizeof(szLocation) - 1, "%s, %s", szCity, szCountry);
            }
            else if (szCountry[0]) {
                strncpy(szLocation, szCountry, sizeof(szLocation) - 1);
            }
            else {
                strncpy(szLocation, "Unknown", sizeof(szLocation) - 1);
            }
            szLocation[sizeof(szLocation) - 1] = '\0';

            extern void PlayerDB_SetCountry(const char* szIP, const char* szCountry);
            PlayerDB_SetCountry(szIP, szCountry);
        }
        /* If status != success (private range, etc.) szLocation stays "". */
    }

    EnterCriticalSection(&_csCache);
    SGeoEntry* p = CacheFind(szIP);
    if (p) {
        strncpy(p->szLocation, szLocation, sizeof(p->szLocation) - 1);
        p->szLocation[sizeof(p->szLocation) - 1] = '\0';
    }
    LeaveCriticalSection(&_csCache);

    /* Signal any SayJoinLocation call that was waiting for this IP. */  /* 1111 */
    _GeoAnnounce_Signal(szIP);                                           /* 1111 */

    return 0;
}

/* ---- Public API -------------------------------------------------------- */

/* ── 1111: Register a pending announcement.
 * Called from SayJoinLocationFunc when GeoIP_GetCached returns GEOIP_PENDING.
 * Reuses an existing entry for the same IP if one already exists. */
static void _GeoAnnounce_Register(                                  /* 1111 */
    INDEX iExclude, const char* szName, const char* szIP)
{
    if (!_bPendingInit) return;
    EnterCriticalSection(&_csPending);

    int iFree = -1;
    for (int i = 0; i < GEOANNOUNCE_MAX; i++) {
        if (_aPending[i].bPending && strcmp(_aPending[i].szIP, szIP) == 0) {
            strncpy(_aPending[i].szName, szName, 255);
            _aPending[i].szName[255] = '\0';
            _aPending[i].iExclude = iExclude;
            LeaveCriticalSection(&_csPending);
            return;
        }
        if (iFree == -1 && !_aPending[i].bPending) iFree = i;
    }
    if (iFree != -1) {
        SGeoAnnounce& a = _aPending[iFree];
        a.bPending = TRUE;
        a.bReady = FALSE;
        a.iExclude = iExclude;
        strncpy(a.szIP, szIP, 15);  a.szIP[15] = '\0';
        strncpy(a.szName, szName, 255); a.szName[255] = '\0';
    }
    LeaveCriticalSection(&_csPending);
}

/* ── 1111: Mark all pending entries for this IP as ready.
 * Called by GeoLookupThread after the result is stored in cache. */
static void _GeoAnnounce_Signal(const char* szIP) {                 /* 1111 */
    if (!_bPendingInit) return;
    EnterCriticalSection(&_csPending);
    for (int i = 0; i < GEOANNOUNCE_MAX; i++) {
        if (_aPending[i].bPending && strcmp(_aPending[i].szIP, szIP) == 0)
            _aPending[i].bReady = TRUE;
    }
    LeaveCriticalSection(&_csPending);
}

/* ── 1111: Cancel any pending announcement for this IP.
 * Call from PlayerDB_OnDisconnect so a departed player's entry never fires
 * for a new joiner who takes the same client slot. */
void GeoIP_ClearPendingByIP(const char* szIP) {                     /* 1111 */
    if (!_bPendingInit || !szIP || !szIP[0]) return;
    EnterCriticalSection(&_csPending);
    for (int i = 0; i < GEOANNOUNCE_MAX; i++) {
        if (_aPending[i].bPending && strcmp(_aPending[i].szIP, szIP) == 0)
            memset(&_aPending[i], 0, sizeof(_aPending[i]));
    }
    LeaveCriticalSection(&_csPending);
}

/* ── 1111: Fire all ready announcements on the main game thread.
 * Snapshots ready entries and releases the lock before any engine calls. */
void GeoIP_FirePendingAnnounces(void) {                             /* 1111 */
    if (!_bPendingInit) return;

    SGeoAnnounce aReady[GEOANNOUNCE_MAX];
    int nReady = 0;

    EnterCriticalSection(&_csPending);
    for (int i = 0; i < GEOANNOUNCE_MAX; i++) {
        if (_aPending[i].bPending && _aPending[i].bReady) {
            aReady[nReady++] = _aPending[i];
            memset(&_aPending[i], 0, sizeof(_aPending[i]));
        }
    }
    LeaveCriticalSection(&_csPending);

    if (nReady == 0 || !_pNetwork->IsServer()) return;

    for (int j = 0; j < nReady; j++) {  /* 1111 — use j; MSVC6 scopes for-int to function */
        SGeoAnnounce& a = aReady[j];

        CTString strLocation = GeoIP_GetCached(a.szIP);

        /* Same city-strip and fallback logic as SayJoinLocationFunc. */
        if (strLocation == "" || strLocation == CTString(GEOIP_PENDING)) {
            strLocation = CTString("somewhere");
        }
        else {
            const char* pComma = strchr(strLocation.str_String, ',');
            if (pComma) strLocation = CTString(pComma + 2);
        }

        CTString strMessage;
        strMessage.PrintF(
            "^c80ffff%s^cffff90 from ^c80ffff%s^cffff90 has joined the game",
            a.szName, strLocation.str_String);

        CServer& srv = _pNetwork->ga_srvServer;
        for (INDEX j = 0; j < srv.srv_assoSessions.Count(); j++) {
            if (j > 0 && !srv.srv_assoSessions[j].sso_bActive) continue;
            if (j == a.iExclude) continue;
            INetwork::SendChatToClient(j, CTString(""), strMessage);
        }
    }
}

void GeoIP_Lookup(const char* szIP) {
    if (!szIP || !szIP[0]) return;
    CacheInit();

    EnterCriticalSection(&_csCache);
    SGeoEntry* p = CacheGetOrCreate(szIP);
    BOOL bAlreadyKnown = (p && p->szLocation[0] != '\0');
    if (!bAlreadyKnown && p) {
        strncpy(p->szLocation, GEOIP_PENDING, sizeof(p->szLocation) - 1);
    }
    LeaveCriticalSection(&_csCache);

    if (bAlreadyKnown || !p) return;

    SGeoWorkItem* pItem = new SGeoWorkItem;
    strncpy(pItem->szIP, szIP, 15);
    pItem->szIP[15] = '\0';

    HANDLE hThread = CreateThread(NULL, 0, GeoLookupThread, pItem, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
    }
    else {
        EnterCriticalSection(&_csCache);
        SGeoEntry* p2 = CacheFind(szIP);
        if (p2) p2->szLocation[0] = '\0';
        LeaveCriticalSection(&_csCache);
        delete pItem;
    }
}

CTString GeoIP_GetCached(const char* szIP) {
    if (!szIP || !szIP[0]) return CTString("");
    CacheInit();

    char szLoc[64];
    szLoc[0] = '\0';
    BOOL bPending = FALSE;

    EnterCriticalSection(&_csCache);
    SGeoEntry* p = CacheFind(szIP);
    if (p) {
        if (strcmp(p->szLocation, GEOIP_PENDING) == 0) {
            bPending = TRUE;
        }
        else if (p->szLocation[0] != '\0') {
            strncpy(szLoc, p->szLocation, sizeof(szLoc) - 1);
            szLoc[sizeof(szLoc) - 1] = '\0';
        }
    }
    LeaveCriticalSection(&_csCache);

    if (bPending) return CTString(GEOIP_PENDING);
    return CTString(szLoc);
}

/* ── 1111: Same cache lookup, split into city/country for the GameSpy
 * responder. Empty strCountry means "not resolved yet" (covers both
 * never-looked-up and still-pending) — caller just omits the fields. */
void GeoIP_GetCachedSplit(const char* szIP, CTString& strCity, CTString& strCountry) {  /* 1111 */
    strCity = "";
    strCountry = "";

    CTString geo = GeoIP_GetCached(szIP);
    if (geo == "" || geo == CTString(GEOIP_PENDING)) return;

    const char* pComma = strchr(geo.str_String, ',');
    if (pComma == NULL) {
        strCountry = geo;
        return;
    }

    char szCity[64];
    INDEX iCityLen = (INDEX)(pComma - geo.str_String);
    if (iCityLen >= (INDEX)sizeof(szCity)) iCityLen = sizeof(szCity) - 1;
    strncpy(szCity, geo.str_String, iCityLen);
    szCity[iCityLen] = '\0';

    strCity = CTString(szCity);
    strCountry = CTString(pComma + 2);  /* skip ", " */
}

/* ---- Shell globals ----------------------------------------------------- */

FLOAT    cmd_fPingResult = -1.0f;  /* 1111 */
CTString cmd_strGeoResult = "";     /* 1111 */
FLOAT    cmd_fNetPingResult = -1.0f;  /* 1111  -1 = pending/unreachable, >=0 = RTT ms */

/* Idle/AFK position-check state (1111) */
INDEX cmd_bPosValid = 0;                    /* 1111 */
FLOAT cmd_fPosX = 0.0f;                     /* 1111 */
FLOAT cmd_fPosY = 0.0f;                     /* 1111 */
FLOAT cmd_fPosZ = 0.0f;                     /* 1111 */
FLOAT cmd_fAngH = 0.0f;                     /* 1111 */
FLOAT cmd_fAngP = 0.0f;                     /* 1111 */
FLOAT cmd_fAngB = 0.0f;                     /* 1111 */

/* Per-slot history + accumulated idle time (index 0 unused, 1..8 = client slots) */
FLOAT cmd_fPrevPosX[9] = { 0 };               /* 1111 */
FLOAT cmd_fPrevPosY[9] = { 0 };               /* 1111 */
FLOAT cmd_fPrevPosZ[9] = { 0 };               /* 1111 */
FLOAT cmd_fPrevAngH[9] = { 0 };               /* 1111 */
FLOAT cmd_fPrevAngP[9] = { 0 };               /* 1111 */
FLOAT cmd_fPrevAngB[9] = { 0 };               /* 1111 */
INDEX cmd_bHasPrevPos[9] = { 0 };             /* 1111 */
FLOAT cmd_fIdleTime[9] = { 0 };               /* 1111 - accumulated seconds spent motionless */

/* Idle watcher config - adjustable from init.ini (1111) */
FLOAT cmd_tmIdleCheckInterval = 300.0f;     /* 1111 - seconds between samples (default: 5 min) */
FLOAT cmd_tmIdleKickAfter = 2700.0f;    /* 1111 - seconds motionless before kick (default: 45 min) */

/* ---- Helper: get player name for a client slot ----------------------- */

/* Returns the decorated name of the first active player on this client slot,
   or "" if no active player is found.
   Uses plb_pcCharacter.GetNameForPrinting() - the same path as StockCommands. */
static CTString GetNameForClient(INDEX iClient) {
    if (!_pNetwork->IsServer()) return CTString("");

    CServer& srv = _pNetwork->ga_srvServer;
    for (INDEX i = 0; i < srv.srv_aplbPlayers.Count(); i++) {
        CPlayerBuffer& plb = srv.srv_aplbPlayers[i];
        if (!plb.IsActive() || plb.plb_iClient != iClient) continue;

        return plb.plb_pcCharacter.GetNameForPrinting();
    }
    return CTString("");
}

/* ---- Shell function: GetClientLocation --------------------------------
 *
 * KEY DESIGN: SE1's shell parser copies the VALUE of a CTString argument
 * onto a temporary stack and passes a pointer to that temp copy.  Writing
 * through that pointer does NOT write back into the caller's script variable.
 *
 * Solution: write the result into the global cmd_strGeoResult instead.
 * The script reads cmd_strGeoResult immediately after calling GetClientLocation().
 *
 * Signature: void GetClientLocation(INDEX iClient)
 * -------------------------------------------------------------------- */

static void GetClientLocationFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    cmd_strGeoResult = "";

    if (!_pNetwork->IsServer()) return;
    if (iClient <= 0) return;

    CServer& srv = _pNetwork->ga_srvServer;
    if (iClient >= (INDEX)srv.srv_assoSessions.Count()) return;
    if (!srv.srv_assoSessions[iClient].sso_bActive) return;

    SClientAddress addr;
    IClientLogging::GetAddress(addr, iClient);
    CTString strIP = addr.GetIPAsString();
    if (strIP == "") return;

    CTString geo = GeoIP_GetCached(strIP.str_String);

    if (geo == CTString(GEOIP_PENDING)) {
        /* Lookup is in progress - tell the script the IP is known but
           location is still resolving so the user sees something useful. */
        cmd_strGeoResult = strIP + " (...)";
    }
    else if (geo != "") {
        cmd_strGeoResult = geo;
    }
    else {
        /* Nothing in cache at all yet - shouldn't normally happen since
           GeoIP_Lookup() is called at join time, but handle it gracefully. */
        cmd_strGeoResult = strIP;
    }
}

/* ---- Shell function: GetClientCountry ---------------------------------
 * Same lookup as GetClientLocation, but writes only the COUNTRY (no city)
 * into cmd_strCountryResult, so join scripts can build their own message.
 * "..." = lookup still pending. "" = inactive slot / no IP.
 * Signature: void GetClientCountry(INDEX iClient)
 * ---------------------------------------------------------------------- */
static void GetClientCountryFunc(SHELL_FUNC_ARGS) {              /* 1111 */
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    cmd_strCountryResult = "";

    if (!_pNetwork->IsServer()) return;
    if (iClient <= 0) return;

    CServer& srv = _pNetwork->ga_srvServer;
    if (iClient >= (INDEX)srv.srv_assoSessions.Count()) return;
    if (!srv.srv_assoSessions[iClient].sso_bActive) return;

    SClientAddress addr;
    IClientLogging::GetAddress(addr, iClient);
    CTString strIP = addr.GetIPAsString();
    if (strIP == "") return;

    CTString geo = GeoIP_GetCached(strIP.str_String);

    if (geo == CTString(GEOIP_PENDING)) {
        cmd_strCountryResult = GEOIP_PENDING;          /* "..." -> script retries/falls back */
        return;
    }
    if (geo == "") {
        GeoIP_Lookup(strIP.str_String);                /* shouldn't normally happen, but cover it */
        cmd_strCountryResult = GEOIP_PENDING;
        return;
    }

    const char* pComma = strchr(geo.str_String, ',');
    cmd_strCountryResult = pComma ? CTString(pComma + 2) : geo;   /* strip "City, " */
}

/* ---- Shell function: GetClientName ------------------------------------ *
 *
 * Returns the player name for a client slot as a CTString return value.
 * The shell supports CTString return values from user functions.
 *
 * Signature: CTString GetClientName(INDEX iClient)
 * -------------------------------------------------------------------- */

static CTString GetClientNameFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);
    return GetNameForClient(iClient);
}

/* ---- Shell function: GetClientPing ------------------------------------ */

static void GetClientPingFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    cmd_fPingResult = -1.0f;

    if (!_pNetwork->IsServer()) return;
    if (iClient <= 0) return;

    CServer& srv = _pNetwork->ga_srvServer;

    for (INDEX i = 0; i < srv.srv_aplbPlayers.Count(); i++) {
        CPlayerBuffer& plb = srv.srv_aplbPlayers[i];
        if (plb.IsActive() && plb.plb_iClient == iClient) {
            cmd_fPingResult = (FLOAT)plb.plb_iPing;
            return;
        }
    }
}

/* ---- Shell function: GetClientPosition --------------------------------
 *
 * Same value-copy constraint as GetClientLocation — results go into
 * globals, not return-by-reference.
 *
 * srv_aplbPlayers[i] and ga_sesSessionState.ses_apltPlayers[i] are
 * parallel arrays by player slot (see upstream Server.cpp's
 * itplb->plb_Index = iPlayer, and GameAgent.cpp's player-status loop
 * for precedent of using them together this way).
 *
 * Signature: void GetClientPosition(INDEX iClient)
 * cmd_bPosValid stays 0 if the slot has no active player or no live
 * entity yet (just connected, intermission, etc).
 * ------------------------------------------------------------------- */
static void GetClientPositionFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    cmd_bPosValid = 0;

    if (!_pNetwork->IsServer()) return;
    if (iClient <= 0) return;

    CServer& srv = _pNetwork->ga_srvServer;

    for (INDEX i = 0; i < srv.srv_aplbPlayers.Count(); i++) {
        CPlayerBuffer& plb = srv.srv_aplbPlayers[i];
        if (!plb.IsActive() || plb.plb_iClient != iClient) continue;

        CPlayerTarget& plt = _pNetwork->ga_sesSessionState.ses_apltPlayers[i];
        if (!plt.plt_bActive || plt.plt_penPlayerEntity == NULL) return;

        const CPlacement3D& pl = plt.plt_penPlayerEntity->GetPlacement();
        cmd_fPosX = pl.pl_PositionVector(1);
        cmd_fPosY = pl.pl_PositionVector(2);
        cmd_fPosZ = pl.pl_PositionVector(3);
        cmd_fAngH = pl.pl_OrientationAngle(1);   /* 1111 */
        cmd_fAngP = pl.pl_OrientationAngle(2);   /* 1111 */
        cmd_fAngB = pl.pl_OrientationAngle(3);   /* 1111 */
        cmd_bPosValid = 1;
        return;
    }
}

/* ---- Shell function: GetNetworkPing ----------------------------------- *   1111
 *
 * Triggers an async ICMP echo to the client's IPv4 address and writes the
 * round-trip time (ms) into cmd_fNetPingResult.
 *
 * On the FIRST call for a new player the lookup is still in flight:
 *   cmd_fNetPingResult == -1  (pending -- call again in ~3 s)
 * On subsequent calls once resolved:
 *   cmd_fNetPingResult >= 0   (RTT in ms)
 *   cmd_fNetPingResult == -1  (ICMP blocked / unreachable)
 *
 * Signature: void GetNetworkPing(INDEX iClient)
 * --------------------------------------------------------------------- */

static void GetNetworkPingFunc(SHELL_FUNC_ARGS) {                     /* 1111 */
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    cmd_fNetPingResult = -1.0f;

    if (!_pNetwork->IsServer()) return;
    if (iClient <= 0) return;

    CServer& srv = _pNetwork->ga_srvServer;
    if (iClient >= (INDEX)srv.srv_assoSessions.Count()) return;
    if (!srv.srv_assoSessions[iClient].sso_bActive) return;

    SClientAddress addr;
    IClientLogging::GetAddress(addr, iClient);
    CTString strIP = addr.GetIPAsString();
    if (strIP == "") return;

    FLOAT fRTT = PingLookup(strIP.str_String);

    cmd_fNetPingResult = (fRTT == PING_PENDING) ? -1.0f : fRTT;
}

static void SayJoinLocationFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iExclude = NEXT_ARG(INDEX);
    const CTString& strName = *NEXT_ARG(CTString*);
    const CTString& strIP = *NEXT_ARG(CTString*);

    CTString strLocation = GeoIP_GetCached(strIP.str_String);

    if (strLocation == CTString(GEOIP_PENDING)) {                        /* 1111 */
        /* Lookup still in flight — defer announcement until it finishes. */  /* 1111 */
        _GeoAnnounce_Register(iExclude,                                  /* 1111 */
            strName.str_String, strIP.str_String);                       /* 1111 */
        return;                                                           /* 1111 */
    }                                                                     /* 1111 */

    if (strLocation == "") {
        GeoIP_Lookup(strIP.str_String);
        strLocation = CTString("somewhere");
    }

    const char* pComma = strchr(strLocation.str_String, ',');
    if (pComma) {
        strLocation = CTString(pComma + 2);
    }

    CTString strMessage;
    strMessage.PrintF("^c80ffff%s^cffff90 from ^c80ffff%s^cffff90 has joined the game",
        strName.str_String, strLocation.str_String);

    CServer& srv = _pNetwork->ga_srvServer;
    for (INDEX i = 0; i < srv.srv_assoSessions.Count(); i++) {
        if (i > 0 && !srv.srv_assoSessions[i].sso_bActive) continue;
        if (i == iExclude) continue;

        INetwork::SendChatToClient(i, CTString("^ceeee"), strMessage);
    }
}

/* ---- Shell function: GetClientCoords ---------------------------------- *
 *
 * Writes the world-space position of the first active player on a given
 * client slot into cmd_strPlayerCoords as "x,y,z" (2 decimal places).
 * Writes "" if the client is inactive or has no entity spawned yet.
 *
 * Signature: void GetClientCoords(INDEX iClient)
 * -------------------------------------------------------------------- */

static void GetClientCoordsFunc(SHELL_FUNC_ARGS) {           /* 1111 */
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    cmd_strPlayerCoords = "";

    if (!_pNetwork->IsServer()) return;
    if (iClient <= 0) return;

    CServer& srv = _pNetwork->ga_srvServer;

    for (INDEX i = 0; i < srv.srv_aplbPlayers.Count(); i++) {
        CPlayerBuffer& plb = srv.srv_aplbPlayers[i];
        if (!plb.IsActive() || plb.plb_iClient != iClient) continue;

        CPlayerEntity* pen =
            _pNetwork->ga_sesSessionState.ses_apltPlayers[i].plt_penPlayerEntity;
        if (pen == NULL) return;

        const FLOAT3D& vPos = pen->GetPlacement().pl_PositionVector;
        cmd_strPlayerCoords.PrintF("%.2f,%.2f,%.2f", vPos(1), vPos(2), vPos(3));
        return;
    }
}

/* ---- Init / Shutdown -------------------------------------------------- */

void GeoIP_Init(void) {
    CacheInit();

    if (!_bPendingInit) {                                               /* 1111 */
        InitializeCriticalSection(&_csPending);                         /* 1111 */
        memset(_aPending, 0, sizeof(_aPending));                        /* 1111 */
        _bPendingInit = TRUE;                                           /* 1111 */
    }                                                                   /* 1111 */

    if (!_bPingInit) {                                                  /* 1111 */
        InitializeCriticalSection(&_csPing);                            /* 1111 */
        memset(_aPingCache, 0, sizeof(_aPingCache));                    /* 1111 */
        _bPingInit = TRUE;                                              /* 1111 */
    }                                                                   /* 1111 */

    _pShell->DeclareSymbol(
        "user void GetClientLocation(INDEX);",
        &GetClientLocationFunc);                                /* 1111 */

    _pShell->DeclareSymbol(
        "user CTString cmd_strCountryResult;",
        &cmd_strCountryResult);                                  /* 1111 */

    _pShell->DeclareSymbol(
        "user void GetClientCountry(INDEX);",
        &GetClientCountryFunc);                                  /* 1111 */

    _pShell->DeclareSymbol(
        "user CTString cmd_strGeoResult;",
        &cmd_strGeoResult);                                     /* 1111 */

    _pShell->DeclareSymbol(
        "user CTString GetClientName(INDEX);",
        &GetClientNameFunc);                                    /* 1111 */

    _pShell->DeclareSymbol(
        "user FLOAT cmd_fPingResult;",
        &cmd_fPingResult);                                      /* 1111 */

    _pShell->DeclareSymbol(
        "user void GetClientPing(INDEX);",
        &GetClientPingFunc);                                    /* 1111 */

    _pShell->DeclareSymbol(
        "user void SayJoinLocation(INDEX, CTString, CTString);",
        &SayJoinLocationFunc);                                    /* 1111 */

    _pShell->DeclareSymbol(
        "user CTString cmd_strPlayerCoords;",
        &cmd_strPlayerCoords);                                /* 1111 */

    _pShell->DeclareSymbol(
        "user void GetClientCoords(INDEX);",
        &GetClientCoordsFunc);                                /* 1111 */

    _pShell->DeclareSymbol(
        "user void GetClientPosition(INDEX);",
        &GetClientPositionFunc);                                /* 1111 */

    _pShell->DeclareSymbol("user INDEX cmd_bPosValid;", &cmd_bPosValid);  /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fAngH;", &cmd_fAngH);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fAngP;", &cmd_fAngP);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fAngB;", &cmd_fAngB);   /* 1111 */

    _pShell->DeclareSymbol("user FLOAT cmd_fPrevAngH[9];", &cmd_fPrevAngH);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fPrevAngP[9];", &cmd_fPrevAngP);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fPrevAngB[9];", &cmd_fPrevAngB);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fIdleTime[9];", &cmd_fIdleTime);   /* 1111 */

    _pShell->DeclareSymbol("user FLOAT cmd_tmIdleCheckInterval;", &cmd_tmIdleCheckInterval);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_tmIdleKickAfter;", &cmd_tmIdleKickAfter);        /* 1111 */

    _pShell->DeclareSymbol("user FLOAT cmd_fPosX;", &cmd_fPosX);      /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fPosY;", &cmd_fPosY);      /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fPosZ;", &cmd_fPosZ);      /* 1111 */

    _pShell->DeclareSymbol("user FLOAT cmd_fPrevPosX[9];", &cmd_fPrevPosX);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fPrevPosY[9];", &cmd_fPrevPosY);   /* 1111 */
    _pShell->DeclareSymbol("user FLOAT cmd_fPrevPosZ[9];", &cmd_fPrevPosZ);   /* 1111 */
    _pShell->DeclareSymbol("user INDEX cmd_bHasPrevPos[9];", &cmd_bHasPrevPos); /* 1111 */

    _pShell->DeclareSymbol(                                       /* 1111 */
        "user FLOAT cmd_fNetPingResult;",
        &cmd_fNetPingResult);                                     /* 1111 */

    _pShell->DeclareSymbol(                                       /* 1111 */
        "user void GetNetworkPing(INDEX);",
        &GetNetworkPingFunc);                                     /* 1111 */

    CPutString("[GeoIP] Initialized.\n");
}

void GeoIP_Shutdown(void) {
    if (_bPendingInit) {                                                /* 1111 */
        EnterCriticalSection(&_csPending);                              /* 1111 */
        memset(_aPending, 0, sizeof(_aPending));                        /* 1111 */
        LeaveCriticalSection(&_csPending);                              /* 1111 */
        DeleteCriticalSection(&_csPending);                             /* 1111 */
        _bPendingInit = FALSE;                                          /* 1111 */
    }                                                                   /* 1111 */

    if (_bPingInit) {                                                   /* 1111 */
        EnterCriticalSection(&_csPing);                                 /* 1111 */
        memset(_aPingCache, 0, sizeof(_aPingCache));                    /* 1111 */
        LeaveCriticalSection(&_csPing);                                 /* 1111 */
        DeleteCriticalSection(&_csPing);                                /* 1111 */
        _bPingInit = FALSE;                                             /* 1111 */
    }                                                                   /* 1111 */

    if (!_bCacheInit) return;
    EnterCriticalSection(&_csCache);
    memset(_aCache, 0, sizeof(_aCache));
    LeaveCriticalSection(&_csCache);
    DeleteCriticalSection(&_csCache);
    _bCacheInit = FALSE;

    CPutString("[GeoIP] Shutdown.\n");
}