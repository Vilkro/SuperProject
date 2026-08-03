/* PlayersBrowse.cpp вЂ” @browse chat command   1111
 *
 * Fetches the public FE (serioussam) and SE (serioussamse) server lists from
 * master.333networks.com, plus per-server player names for populated servers,
 * and reports the result back to whichever client asked, via chat.
 *
 * Runs entirely on a background thread (CreateThread), matching GeoIP.cpp's
 * established pattern in this codebase: NO CTString (or any other engine-
 * managed type) is touched from that thread вЂ” only plain C strings, raw
 * WinHTTP calls, and a CRITICAL_SECTION-protected malloc'd buffer to hand the
 * finished result back to the main thread. CTString is only ever constructed
 * on the main thread, inside BrowseCheckResultFunc, which is a normal shell
 * function call.
 *
 * WinHTTP linking mirrors HttpRequests.cpp exactly: direct-link when
 * SE1_VER == SE1_110 (winhttp.h is available), otherwise load Winhttp.dll at
 * runtime via LoadLibrary/GetProcAddress with locally-declared function
 * pointers, since older toolchains (VC98) don't ship winhttp.h at all.
 * <WinInet.h> alone (no winhttp.h) supplies HINTERNET/INTERNET_PORT/etc. and
 * has been part of the SDK since the VC98 era, so it's safe on either branch.
 *
 * Windows-only, same as GeoIP.cpp / HttpRequests.cpp elsewhere in this patch.
 *
 * Relies on WSAStartup() already having been called - QueryManager.cpp's
 * InitQuery() does this, and runs before PlayersBrowse_Init() in
 * INetwork::Initialize(), so no separate WSAStartup() call is needed here.
 *
 * To add to the Core project:
 *   Add Core\Query\PlayersBrowse.cpp to ClCompile in Core.vcxproj
 *   (same as PlayerDB.cpp / GeoIP.cpp). No additional .lib needed beyond
 *   whatever HttpRequests.cpp already pulls in for your SE1_VER branch.
 *
 * Chat command wiring (add to MessageProcessing.cpp's OnChatInRequest,
 * alongside the existing @kick/@ban checks):
 *   if (strMessage == "@browse") { return HandleBrowseCommand(iClient); }
 *
 * Shell symbols declared by PlayersBrowse_Init():
 *   void BrowsePublicServers(INDEX iClient)  - starts a browse, replies "busy"
 *                                               if one is already running
 *   void BrowseCheckResult(INDEX iAttempt)   - internal poll, scheduled via
 *                                               IScriptScheduler every 1s
 */

#include "StdH.h"

#include "PlayersBrowse.h"
#include "Networking/NetworkFunctions.h"
#include "ScriptScheduler.h"

#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

/* ---- WinHTTP linking (mirrors HttpRequests.cpp exactly) ----------------- */

#if SE1_VER == SE1_110
  // Link library directly
  #include <winhttp.h>
  #pragma comment(lib, "Winhttp.lib")

  static __forceinline BOOL PB_LinkWinHttp(void) {
    return TRUE;
  };

#else
  // Get functions from the library - no winhttp.h needed, WinInet.h alone
  // supplies the types (HINTERNET, INTERNET_PORT, INTERNET_DEFAULT_HTTPS_PORT).
  #include <WinInet.h>

  #define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
  #define WINHTTP_NO_PROXY_NAME   NULL
  #define WINHTTP_NO_PROXY_BYPASS NULL
  #define WINHTTP_FLAG_SECURE 0x00800000

  #define WINHTTP_NO_ADDITIONAL_HEADERS NULL
  #define WINHTTP_NO_REQUEST_DATA       NULL

  typedef ULONG DWORD_PTR;

  static HINTERNET (__stdcall *WinHttpOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
  static HINTERNET (__stdcall *WinHttpConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
  static HINTERNET (__stdcall *WinHttpOpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
  static BOOL (__stdcall *WinHttpSendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
  static BOOL (__stdcall *WinHttpReceiveResponse)(HINTERNET, LPVOID);
  static BOOL (__stdcall *WinHttpQueryDataAvailable)(HINTERNET, LPDWORD);
  static BOOL (__stdcall *WinHttpReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
  static BOOL (__stdcall *WinHttpCloseHandle)(HINTERNET);

  static __forceinline BOOL PB_LinkWinHttp(void) {
    // Load methods from the library dynamically
    HINSTANCE hWinHttp = LoadLibraryA("Winhttp.dll");

    // Couldn't initialize
    if (hWinHttp == NULL) return FALSE;

    typedef HINTERNET (__stdcall *COpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    typedef HINTERNET (__stdcall *CConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
    typedef HINTERNET (__stdcall *COpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
    typedef BOOL (__stdcall *CSendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    typedef BOOL (__stdcall *CResponse)(HINTERNET, LPVOID);
    typedef BOOL (__stdcall *CDataAvailable)(HINTERNET, LPDWORD);
    typedef BOOL (__stdcall *CReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
    typedef BOOL (__stdcall *CCloseHandle)(HINTERNET);

    WinHttpOpen               = (COpen)GetProcAddress(hWinHttp, "WinHttpOpen");
    WinHttpConnect            = (CConnect)GetProcAddress(hWinHttp, "WinHttpConnect");
    WinHttpOpenRequest        = (COpenRequest)GetProcAddress(hWinHttp, "WinHttpOpenRequest");
    WinHttpSendRequest        = (CSendRequest)GetProcAddress(hWinHttp, "WinHttpSendRequest");
    WinHttpReceiveResponse    = (CResponse)GetProcAddress(hWinHttp, "WinHttpReceiveResponse");
    WinHttpQueryDataAvailable = (CDataAvailable)GetProcAddress(hWinHttp, "WinHttpQueryDataAvailable");
    WinHttpReadData           = (CReadData)GetProcAddress(hWinHttp, "WinHttpReadData");
    WinHttpCloseHandle        = (CCloseHandle)GetProcAddress(hWinHttp, "WinHttpCloseHandle");

    // Couldn't initialize
    if (WinHttpOpen == NULL || WinHttpConnect == NULL || WinHttpOpenRequest == NULL || WinHttpSendRequest == NULL
     || WinHttpReceiveResponse == NULL || WinHttpQueryDataAvailable == NULL || WinHttpReadData == NULL || WinHttpCloseHandle == NULL) {
      return FALSE;
    }

    return TRUE;
  };
#endif

static BOOL _bWinHttpLinked = FALSE;

/* ---- Minimal WinHTTPS GET into a malloc'd raw buffer -------------------
 * Deliberately avoids CTString / HttpRequests.cpp's CHttpResponse: CTString's
 * allocator is not verified safe to touch from a background thread in this
 * engine, which is exactly why GeoIP.cpp's own worker thread never uses it
 * either (see GeoIP.cpp's HttpGet/JsonGetString - same reasoning). Only the
 * WinHTTP calls themselves differ between the two link modes above; this
 * function's body is identical either way since both branches expose the
 * same function names. */
static BOOL HttpsGetRaw(const wchar_t* wszHost, const wchar_t* wszPath, char** ppszOut) {
    *ppszOut = NULL;
    if (!_bWinHttpLinked) return FALSE;

    HINTERNET hSession = WinHttpOpen(L"SamPlayersBrowse/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return FALSE;

    HINTERNET hConnect = WinHttpConnect(hSession, wszHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return FALSE; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wszPath, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return FALSE; }

    BOOL bOk = FALSE;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hRequest, NULL)) {

        size_t iTotal = 0;
        char* pBuf = (char*)malloc(1);
        if (pBuf) pBuf[0] = '\0';

        DWORD ulAvail = 0;
        while (pBuf && WinHttpQueryDataAvailable(hRequest, &ulAvail) && ulAvail > 0) {
            char* pNew = (char*)realloc(pBuf, iTotal + ulAvail + 1);
            if (!pNew) break;
            pBuf = pNew;

            DWORD ulRead = 0;
            if (!WinHttpReadData(hRequest, pBuf + iTotal, ulAvail, &ulRead)) break;
            iTotal += ulRead;
            pBuf[iTotal] = '\0';
        }

        if (pBuf && iTotal > 0) {
            *ppszOut = pBuf;
            bOk = TRUE;
        } else if (pBuf) {
            free(pBuf);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return bOk;
}

/* ---- Minimal JSON helpers (raw C strings only, no nesting library) ----- */

/* Finds "key":"value" anywhere in szJson. Writes up to nOut-1 chars. */
static BOOL JsonGetString(const char* szJson, const char* szKey, char* szOut, int nOut) {
    char szPattern[64];
    _snprintf(szPattern, sizeof(szPattern) - 1, "\"%s\":\"", szKey);
    szPattern[sizeof(szPattern) - 1] = '\0';

    const char* p = strstr(szJson, szPattern);
    if (!p) return FALSE;
    p += strlen(szPattern);

    const char* e = strchr(p, '"');
    if (!e) return FALSE;

    int iLen = (int)(e - p);
    if (iLen >= nOut) iLen = nOut - 1;
    memcpy(szOut, p, iLen);
    szOut[iLen] = '\0';
    return TRUE;
}

/* Finds "key":123 (unquoted numeric value) anywhere in szJson. */
static BOOL JsonGetInt(const char* szJson, const char* szKey, int* piOut) {
    char szPattern[64];
    _snprintf(szPattern, sizeof(szPattern) - 1, "\"%s\":", szKey);
    szPattern[sizeof(szPattern) - 1] = '\0';

    const char* p = strstr(szJson, szPattern);
    if (!p) return FALSE;
    p += strlen(szPattern);
    while (*p == ' ') p++;

    *piOut = atoi(p);
    return TRUE;
}

/* Scans from *ppCursor for the next {...} object (brace-depth aware, skips
 * over quoted-string contents so braces inside e.g. a hostname don't confuse
 * it) whose raw text contains szRequiredKeyPattern. Copies it into szOut and
 * advances *ppCursor past it. Shape-agnostic: doesn't assume how the list
 * response wraps its server array, just finds objects that look like servers.
 * Returns FALSE once there are no more matches. */
static BOOL JsonNextObjectContaining(const char** ppCursor, const char* szRequiredKeyPattern,
                                      char* szOut, int nOut) {
    const char* p = *ppCursor;

    for (;;) {
        while (*p != '{' && *p != '\0') p++;
        if (*p == '\0') { *ppCursor = p; return FALSE; }

        const char* start = p;
        int iDepth = 0;
        BOOL bInString = FALSE;

        do {
            if (!bInString) {
                if (*p == '{') iDepth++;
                else if (*p == '}') iDepth--;
                else if (*p == '"') bInString = TRUE;
            } else {
                if (*p == '\\' && *(p + 1) != '\0') p++;
                else if (*p == '"') bInString = FALSE;
            }
            if (*p == '\0') { *ppCursor = p; return FALSE; }
            p++;
        } while (iDepth > 0);

        int iLen = (int)(p - start);
        int iCopyLen = (iLen < nOut - 1) ? iLen : nOut - 1;
        memcpy(szOut, start, iCopyLen);
        szOut[iCopyLen] = '\0';

        *ppCursor = p;
        if (strstr(szOut, szRequiredKeyPattern) != NULL) return TRUE;
        /* else: not a match (e.g. the trailing meta object) - keep scanning */
    }
}

#define BROWSE_HOST                L"master.333networks.com"
#define BROWSE_MAX_SERVER_QUERIES  120   /* safety cap on direct UDP queries, across both games */
#define BROWSE_OBJ_BUF             4096
#define BROWSE_STATUS_BUF          2048  /* raw \key\value\ reply from a server's own query port */

/* One server we intend to (or did) query directly. Filled in during the
 * JSON-list pass, sent during the batch-send pass, matched against incoming
 * replies during the batch-receive pass. */
struct SPendingServer {
    int iGame; /* 0 = FE, 1 = SE */
    char szHostnameFallback[512]; /* from JSON, already UTF8->CP1251 converted */
    char szMapnameFallback[384];  /* from JSON, already UTF8->CP1251 converted */
    int iListedPlayers, iListedMax;
    struct sockaddr_in sinTarget;
    BOOL bSent;
    BOOL bResponded;
    char szStatus[BROWSE_STATUS_BUF];
};

/* ---- Direct UDP status query, batched (same protocol as the in-game
 * browser, but not one-request-at-a-time) ----------------------------------
 * 333networks' JSON *list* is fine for discovering which IP:port pairs
 * exist, but its per-server *detail* is a cache that only refreshes every
 * several minutes. The in-game "Join Internet Server" browser sidesteps that
 * entirely: it gets the address list from the master, then asks each server
 * directly for its live status - the exact "\status\" query your own
 * LegacyServerQuery.cpp already answers on the query port (game port + 1).
 * This does the same thing, so @browse shows what's actually happening on a
 * server right now instead of what 333networks last polled.
 *
 * Doing that one server at a time (sendto, block on recvfrom with a 1s
 * timeout, repeat) means every listed server that's no longer actually
 * running costs a full second, one after another - a handful of stale
 * entries and the whole thing crawls. So instead: one non-blocking socket,
 * every "\status\" query sent up front, then a single shared receive window
 * where responses get matched back to their sender by address as they
 * arrive. Total worst case is bounded by BROWSE_UDP_BUDGET_MS regardless of
 * how many servers are being asked, and it finishes early the moment
 * everyone's answered instead of always waiting out a fixed timeout.
 *
 * Plain winsock throughout, no engine calls - same thread-safety reasoning
 * as HttpsGetRaw. */
#define BROWSE_UDP_BUDGET_MS 1500

static void UdpStatusQueryBatch(SPendingServer* paServers, int ctServers) {
    if (ctServers <= 0) return;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    u_long ulNonBlocking = 1;
    ioctlsocket(s, FIONBIO, &ulNonBlocking);

    const char szQuery[] = "\\status\\";
    int ctSent = 0;

    for (int i = 0; i < ctServers; i++) {
        if (paServers[i].sinTarget.sin_addr.s_addr == INADDR_NONE) continue;
        if (sendto(s, szQuery, (int)strlen(szQuery), 0,
                    (struct sockaddr*)&paServers[i].sinTarget, sizeof(struct sockaddr_in)) != SOCKET_ERROR) {
            paServers[i].bSent = TRUE;
            ctSent++;
        }
    }

    if (ctSent == 0) {
        closesocket(s);
        return;
    }

    DWORD dwStart = GetTickCount();
    int ctResponded = 0;

    while (ctResponded < ctSent) {
        DWORD dwElapsed = GetTickCount() - dwStart;
        if (dwElapsed >= BROWSE_UDP_BUDGET_MS) break;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);

        DWORD dwRemaining = BROWSE_UDP_BUDGET_MS - dwElapsed;
        struct timeval tv;
        tv.tv_sec = dwRemaining / 1000;
        tv.tv_usec = (dwRemaining % 1000) * 1000;

        int iSel = select(0, &fds, NULL, NULL, &tv);
        if (iSel <= 0) break; /* timed out, or a real error - either way, stop waiting */

        struct sockaddr_in sinFrom;
        int iFromLen = sizeof(sinFrom);
        char szBuf[BROWSE_STATUS_BUF];
        int iRecv = recvfrom(s, szBuf, sizeof(szBuf) - 1, 0, (struct sockaddr*)&sinFrom, &iFromLen);
        if (iRecv <= 0) continue;
        szBuf[iRecv] = '\0';

        for (int i = 0; i < ctServers; i++) {
            if (!paServers[i].bSent || paServers[i].bResponded) continue;
            if (paServers[i].sinTarget.sin_addr.s_addr == sinFrom.sin_addr.s_addr &&
                paServers[i].sinTarget.sin_port == sinFrom.sin_port) {
                strncpy(paServers[i].szStatus, szBuf, sizeof(paServers[i].szStatus) - 1);
                paServers[i].szStatus[sizeof(paServers[i].szStatus) - 1] = '\0';
                paServers[i].bResponded = TRUE;
                ctResponded++;
                break;
            }
        }
    }

    closesocket(s);
}

/* Finds \key\ in a GameSpy-style backslash-delimited response (the format
 * LegacyServerQuery.cpp's status replies use) and copies the value that
 * follows, up to the next backslash, into szOut. */
static BOOL GsGetString(const char* szData, const char* szKey, char* szOut, int nOut) {
    char szPattern[64];
    _snprintf(szPattern, sizeof(szPattern) - 1, "\\%s\\", szKey);
    szPattern[sizeof(szPattern) - 1] = '\0';

    const char* p = strstr(szData, szPattern);
    if (!p) return FALSE;
    p += strlen(szPattern);

    const char* e = strchr(p, '\\');
    if (!e) e = p + strlen(p);

    int iLen = (int)(e - p);
    if (iLen >= nOut) iLen = nOut - 1;
    memcpy(szOut, p, iLen);
    szOut[iLen] = '\0';
    return TRUE;
}

static BOOL GsGetInt(const char* szData, const char* szKey, int* piOut) {
    char szVal[16];
    if (!GsGetString(szData, szKey, szVal, sizeof(szVal))) return FALSE;
    *piOut = atoi(szVal);
    return TRUE;
}

/* ---- UTF-8 -> Windows-1251 (worker thread safe: plain Win32 codepage API,
 * not an engine call) --------------------------------------------------------
 * 333networks' JSON is UTF-8, so Cyrillic text in it is multi-byte and shows
 * up as garbage/'?' once printed through the game's single-byte CP1251 font.
 * This only belongs on text that came from that JSON (hostname, and mapname
 * when there's no live value yet) - it must NEVER be applied to text that
 * came back from a direct UDP query to another Sam server, since that text
 * is already in the server's native encoding (CP1251 for Cyrillic, same as
 * your own source files), not UTF-8. Running already-correct CP1251 bytes
 * through a UTF-8 decoder would corrupt them instead of fixing anything. */
static void Utf8ToCp1251(const char* szUtf8, char* szOut, int nOut) {
    if (nOut <= 0) return;
    szOut[0] = '\0';

    int cchWide = MultiByteToWideChar(CP_UTF8, 0, szUtf8, -1, NULL, 0);
    if (cchWide <= 0) {
        strncpy(szOut, szUtf8, nOut - 1);
        szOut[nOut - 1] = '\0';
        return;
    }

    wchar_t* wszBuf = (wchar_t*)malloc(cchWide * sizeof(wchar_t));
    if (!wszBuf) {
        strncpy(szOut, szUtf8, nOut - 1);
        szOut[nOut - 1] = '\0';
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, szUtf8, -1, wszBuf, cchWide);
    WideCharToMultiByte(1251, 0, wszBuf, -1, szOut, nOut - 1, "?", NULL);
    szOut[nOut - 1] = '\0';

    free(wszBuf);
}

/* ---- Growable raw output buffer (worker thread only) -------------------- */

static void AppendStr(char** ppBuf, size_t* pCap, size_t* pLen, const char* szAdd) {
    size_t iAddLen = strlen(szAdd);
    if (*pLen + iAddLen + 1 > *pCap) {
        size_t iNewCap = (*pCap) * 2;
        while (iNewCap < *pLen + iAddLen + 1) iNewCap *= 2;
        char* pNew = (char*)realloc(*ppBuf, iNewCap);
        if (!pNew) return; /* best effort; drop the append rather than crash */
        *ppBuf = pNew;
        *pCap = iNewCap;
    }
    memcpy(*ppBuf + *pLen, szAdd, iAddLen + 1); /* +1 copies the null too */
    *pLen += iAddLen;
}

/* ---- Shared result, handed from worker thread to main thread ----------- */

struct SBrowseState {
    BOOL  bBusy;
    BOOL  bReady;
    INDEX iRequester;
    char* pszOutput; /* owned by this struct once bReady is TRUE */
};

static SBrowseState     _browse = { FALSE, FALSE, -1, NULL };
static CRITICAL_SECTION _csBrowse;

/* ---- Worker thread ------------------------------------------------------ */

struct SBrowseGame { const char* szLabel; const char* szGameName; const char* szFullName; };
static const SBrowseGame _aBrowseGames[2] = {
    { "FE", "serioussam",   "The First Encounter" },
    { "SE", "serioussamse", "The Second Encounter" },
};

/* Every line handed back to the main thread is tagged with a 1-char type +
 * '|' prefix so BrowseCheckResultFunc knows how to color/undecorate/
 * translate it without guessing from formatting alone:
 *   H = section header - our own text, sent as "EN|RU" (main thread splits
 *       and picks via SelectLangMessage)
 *   E = "no players online" - our own text, "EN|RU" same as H
 *   N = a note about a server (unreachable, no names, etc.) - our own text,
 *       "EN|RU" same as H, but rendered indented like a player line
 *   S = server hostname + slot count - player-supplied data, undecorated,
 *       never translated
 *   M = map name - player-supplied data, undecorated, never translated
 *   P = a player name - player-supplied data, undecorated, never translated
 */
static DWORD WINAPI BrowseThreadProc(LPVOID lpParam) {
    size_t iCap = 4096;
    size_t iLen = 0;
    char* pOut = (char*)malloc(iCap);
    if (pOut) pOut[0] = '\0';

    BOOL abAnyGame[2]     = { FALSE, FALSE }; /* found >=1 populated server */
    BOOL abFetchFailed[2] = { FALSE, FALSE }; /* couldn't reach master at all */

    size_t aiGameCap[2] = { 4096, 4096 };
    size_t aiGameLen[2] = { 0, 0 };
    char* apGameBuf[2];
    apGameBuf[0] = (char*)malloc(aiGameCap[0]);
    apGameBuf[1] = (char*)malloc(aiGameCap[1]);
    if (apGameBuf[0]) apGameBuf[0][0] = '\0';
    if (apGameBuf[1]) apGameBuf[1][0] = '\0';

    /* ---- Pass 1: collect every listed server from both games, no UDP yet ---- */
    SPendingServer* paPending = (SPendingServer*)calloc(BROWSE_MAX_SERVER_QUERIES, sizeof(SPendingServer));
    int ctPending = 0;

    for (int g = 0; g < 2 && pOut && apGameBuf[g] && paPending; g++) {
        char szPath[128];
        _snprintf(szPath, sizeof(szPath) - 1, "/json/%s", _aBrowseGames[g].szGameName);
        szPath[sizeof(szPath) - 1] = '\0';

        wchar_t wszPath[128];
        mbstowcs(wszPath, szPath, 128);

        char* pList = NULL;
        if (!HttpsGetRaw(BROWSE_HOST, wszPath, &pList) || pList == NULL) {
            abFetchFailed[g] = TRUE;
            if (pList) free(pList);
            continue;
        }

        const char* pCursor = pList;
        char szObj[BROWSE_OBJ_BUF];

        while (ctPending < BROWSE_MAX_SERVER_QUERIES &&
               JsonNextObjectContaining(&pCursor, "\"hostname\"", szObj, sizeof(szObj))) {
            SPendingServer* p = &paPending[ctPending];
            memset(p, 0, sizeof(*p));
            p->iGame = g;

            char szHostnameUtf8[512] = "";
            JsonGetString(szObj, "hostname", szHostnameUtf8, sizeof(szHostnameUtf8));
            Utf8ToCp1251(szHostnameUtf8, p->szHostnameFallback, sizeof(p->szHostnameFallback));

            char szMapnameUtf8[384] = "";
            JsonGetString(szObj, "mapname", szMapnameUtf8, sizeof(szMapnameUtf8));
            Utf8ToCp1251(szMapnameUtf8, p->szMapnameFallback, sizeof(p->szMapnameFallback));

            char szIp[64] = "";
            JsonGetString(szObj, "ip", szIp, sizeof(szIp));

            int iPort = 0, iQueryPort = 0;
            JsonGetInt(szObj, "hostport", &iPort);
            JsonGetInt(szObj, "queryport", &iQueryPort);
            if (iQueryPort <= 0) iQueryPort = iPort + 1; /* fallback if the field's ever missing */

            JsonGetInt(szObj, "numplayers", &p->iListedPlayers);
            JsonGetInt(szObj, "maxplayers", &p->iListedMax);

            /* Strip IPv4-mapped IPv6 notation ("::ffff:1.2.3.4" -> "1.2.3.4") */
            char* pCleanIp = strstr(szIp, "::ffff:");
            pCleanIp = pCleanIp ? pCleanIp + 7 : szIp;

            memset(&p->sinTarget, 0, sizeof(p->sinTarget));
            p->sinTarget.sin_family = AF_INET;
            p->sinTarget.sin_port = htons((u_short)iQueryPort);
            p->sinTarget.sin_addr.s_addr = inet_addr(pCleanIp); /* INADDR_NONE if malformed - skipped when sending */

            ctPending++;
        }

        free(pList);
    }

    /* ---- Pass 2: one shared UDP batch across both games ---- */
    if (paPending && ctPending > 0) {
        UdpStatusQueryBatch(paPending, ctPending);
    }

    /* ---- Pass 3: build the output using live data where we got it,
     * falling back to the (converted) JSON values otherwise ---- */
    for (int i = 0; paPending && i < ctPending; i++) {
        SPendingServer* p = &paPending[i];
        int g = p->iGame;
        if (!apGameBuf[g]) continue;

        char szHostname[512], szMapname[384];
        strncpy(szHostname, p->szHostnameFallback, sizeof(szHostname) - 1);
        szHostname[sizeof(szHostname) - 1] = '\0';
        strncpy(szMapname, p->szMapnameFallback, sizeof(szMapname) - 1);
        szMapname[sizeof(szMapname) - 1] = '\0';

        int iPlayers = p->iListedPlayers, iMax = p->iListedMax;

        if (p->bResponded) {
            int iLivePlayers, iLiveMax;
            if (GsGetInt(p->szStatus, "numplayers", &iLivePlayers)) iPlayers = iLivePlayers;
            if (GsGetInt(p->szStatus, "maxplayers", &iLiveMax))     iMax     = iLiveMax;

            /* Live hostname/mapname come straight from the other server's
             * own memory - same native encoding as your own strings, NOT
             * UTF-8, so they must not go through Utf8ToCp1251 (unlike the
             * JSON fallback above, which is UTF-8 and needs it). This is
             * also *why* the in-game browser shows Cyrillic correctly: it
             * never touches 333networks' JSON text for hostname/mapname at
             * all - only the compact IP:port list comes from the master,
             * and every bit of descriptive text is fetched live, directly,
             * same as here. */
            char szLiveHostname[512];
            if (GsGetString(p->szStatus, "hostname", szLiveHostname, sizeof(szLiveHostname)) && szLiveHostname[0]) {
                strncpy(szHostname, szLiveHostname, sizeof(szHostname) - 1);
                szHostname[sizeof(szHostname) - 1] = '\0';
            }

            char szLiveMapname[384];
            if (GsGetString(p->szStatus, "mapname", szLiveMapname, sizeof(szLiveMapname)) && szLiveMapname[0]) {
                strncpy(szMapname, szLiveMapname, sizeof(szMapname) - 1);
                szMapname[sizeof(szMapname) - 1] = '\0';
            }
        }

        if (iPlayers <= 0) continue; /* only servers with players online */

        abAnyGame[g] = TRUE;

        char szLine[600];
        _snprintf(szLine, sizeof(szLine) - 1, "S|%s  ^cefefef%d/%d\n", szHostname, iPlayers, iMax);
        szLine[sizeof(szLine) - 1] = '\0';
        AppendStr(&apGameBuf[g], &aiGameCap[g], &aiGameLen[g], szLine);

        if (szMapname[0]) {
            char szMapLine[420];
            _snprintf(szMapLine, sizeof(szMapLine) - 1, "M|%s\n", szMapname);
            szMapLine[sizeof(szMapLine) - 1] = '\0';
            AppendStr(&apGameBuf[g], &aiGameCap[g], &aiGameLen[g], szMapLine);
        }

        if (!p->bResponded) {
            AppendStr(&apGameBuf[g], &aiGameCap[g], &aiGameLen[g],
                "N|(couldn't reach server directly for player names)|"
                "(не удалось напрямую связаться с сервером для получения списка игроков)\n");
            continue;
        }

        BOOL bAnyName = FALSE;
        for (int pl = 0; pl < iPlayers && pl < 64; pl++) {
            char szKey[16];
            _snprintf(szKey, sizeof(szKey) - 1, "player_%d", pl);
            szKey[sizeof(szKey) - 1] = '\0';

            char szName[256] = "";
            if (GsGetString(p->szStatus, szKey, szName, sizeof(szName)) && szName[0]) {
                char szNameLine[320];
                _snprintf(szNameLine, sizeof(szNameLine) - 1, "P|%s\n", szName);
                szNameLine[sizeof(szNameLine) - 1] = '\0';
                AppendStr(&apGameBuf[g], &aiGameCap[g], &aiGameLen[g], szNameLine);
                bAnyName = TRUE;
            }
        }

        if (!bAnyName) {
            AppendStr(&apGameBuf[g], &aiGameCap[g], &aiGameLen[g],
                "N|(couldn't fetch player names)|(не удалось получить список игроков)\n");
        }
    }

    if (paPending) free(paPending);

    /* Assemble the final output: drop a game's section entirely when it had
     * no populated servers and the fetch itself worked fine; if neither game
     * has anything to show, say so once instead of printing two empty
     * sections. A fetch failure still gets shown, since that's worth knowing
     * about rather than silently looking identical to "nobody online". */
    if (pOut) {
        BOOL bShowAny = abAnyGame[0] || abAnyGame[1] || abFetchFailed[0] || abFetchFailed[1];

        if (!bShowAny) {
            AppendStr(&pOut, &iCap, &iLen, "E|No players online right now.|Сейчас никто не играет.\n");
        } else {
            for (int g = 0; g < 2; g++) {
                if (!abAnyGame[g] && !abFetchFailed[g]) continue; /* clean empty game - skip */

                char szHeader[192];
                _snprintf(szHeader, sizeof(szHeader) - 1, "H|List of %s players:|Список игроков %s:\n",
                    _aBrowseGames[g].szFullName, _aBrowseGames[g].szFullName);
                szHeader[sizeof(szHeader) - 1] = '\0';
                AppendStr(&pOut, &iCap, &iLen, szHeader);

                if (abFetchFailed[g]) {
                    AppendStr(&pOut, &iCap, &iLen, "N|(couldn't reach master server)|(не удалось связаться с мастер-сервером)\n");
                } else if (apGameBuf[g]) {
                    AppendStr(&pOut, &iCap, &iLen, apGameBuf[g]);
                }
            }
        }
    }

    if (apGameBuf[0]) free(apGameBuf[0]);
    if (apGameBuf[1]) free(apGameBuf[1]);

    EnterCriticalSection(&_csBrowse);
    if (_browse.pszOutput) free(_browse.pszOutput);
    _browse.pszOutput = pOut; /* may be NULL if the very first malloc failed */
    _browse.bReady = TRUE;
    LeaveCriticalSection(&_csBrowse);

    return 0;
}

/* ---- Shell functions (main thread only) --------------------------------- */

#define BROWSE_MAX_POLLS 40 /* ~40 seconds before giving up */

static void BrowsePublicServersFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iClient = NEXT_ARG(INDEX);

    if (!_pNetwork->IsServer()) return;

    if (!_bWinHttpLinked) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80Server browse is unavailable (WinHTTP not linked).^r",
            "^c00ff80Поиск серверов недоступен (WinHTTP не подключен).^r"));
        return;
    }

    BOOL bAlreadyBusy = FALSE;
    EnterCriticalSection(&_csBrowse);
    if (_browse.bBusy) {
        bAlreadyBusy = TRUE;
    } else {
        _browse.bBusy = TRUE;
        _browse.bReady = FALSE;
        _browse.iRequester = iClient;
    }
    LeaveCriticalSection(&_csBrowse);

    if (bAlreadyBusy) {
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80A server browse is already running, please wait...^r",
            "^c00ff80Поиск серверов уже выполняется, подождите...^r"));
        return;
    }

    HANDLE hThread = CreateThread(NULL, 0, BrowseThreadProc, NULL, 0, NULL);
    if (hThread == NULL) {
        EnterCriticalSection(&_csBrowse);
        _browse.bBusy = FALSE;
        LeaveCriticalSection(&_csBrowse);
        INetwork::SendChatToClient(iClient, "^cffff", SelectLangMessage(iClient,
            "^c00ff80Could not start server browse.^r",
            "^c00ff80Не удалось запустить поиск серверов.^r"));
        return;
    }
    CloseHandle(hThread);

    //INetwork::SendChatToClient(iClient, "^cffff", "^c00ff80Fetching public FE/SE server list...^r");
    IScriptScheduler::Schedule(0.01f, "SayToClientLang(cmd_iChatClient, \"^cffff\", \"^CFetching public FE/SE server list...^r\", \"^CПолучение списка публичных серверов FE/SE...^r\");");

    IScriptScheduler::Schedule(1.0f, "BrowseCheckResult(1);");
};

static void BrowseCheckResultFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iAttempt = NEXT_ARG(INDEX);

    if (!_pNetwork->IsServer()) return;

    BOOL bReady, bBusy;
    INDEX iRequester;
    char* pszOutput = NULL;

    EnterCriticalSection(&_csBrowse);
    bReady = _browse.bReady;
    bBusy = _browse.bBusy;
    iRequester = _browse.iRequester;
    if (bReady && _browse.pszOutput) {
        pszOutput = _strdup(_browse.pszOutput);
    }
    LeaveCriticalSection(&_csBrowse);

    if (bReady) {
        if (pszOutput != NULL) {
            char* pLine = strtok(pszOutput, "\n");
            while (pLine != NULL) {
                /* Every line is tagged "X|body" by the worker thread - see the
                 * comment above BrowseThreadProc. CTString/Undecorated() only
                 * happen here, on the main thread, never on the worker. */
                if (pLine[0] != '\0' && pLine[1] == '|') {
                    const char cTag = pLine[0];
                    const char* szBody = pLine + 2;
                    CTString strLine;

                    switch (cTag) {
                        case 'H': /* our own header text - not player-supplied */
                        case 'E': /* "no players online" - our own text */
                        case 'N': { /* a note about a server (e.g. unreachable) - our own text */
                            /* Worker thread can't call SelectLangMessage (CTString/engine
                             * call), so it emits "EN|RU" and this picks the right half. */
                            char szBodyCopy[600];
                            strncpy(szBodyCopy, szBody, sizeof(szBodyCopy) - 1);
                            szBodyCopy[sizeof(szBodyCopy) - 1] = '\0';

                            char* pSep = strchr(szBodyCopy, '|');
                            if (pSep) *pSep = '\0';
                            const char* szEN = szBodyCopy;
                            const char* szRU = pSep ? (pSep + 1) : szBodyCopy;

                            const CTString& strPicked = SelectLangMessage(iRequester, CTString(szEN), CTString(szRU));

                            if (cTag == 'N') {
                                strLine.PrintF("^c80ffff  %s^r", strPicked.str_String); /* same placement as a player note */
                            } else {
                                strLine.PrintF("^c00ff80%s^r", strPicked.str_String);
                            }
                            break;
                        }
                        case 'S': /* server hostname + slots - undecorate it */
                            strLine.PrintF("^cffff80%s^r", CTString(szBody).Undecorated().str_String);
                            break;
                        case 'M': /* map name - undecorate it */
                            strLine.PrintF("^cffff80 %s^r", CTString(szBody).Undecorated().str_String);
                            break;
                        case 'P': /* player name - always raw data, never translated */
                            strLine.PrintF("^cffda59  %s^r", CTString(szBody).Undecorated().str_String);
                            break;
                        default:
                            strLine.PrintF("^c00ff80%s^r", szBody);
                            break;
                    }

                    INetwork::SendChatToClient(iRequester, "^ced2675", strLine);
                }
                pLine = strtok(NULL, "\n");
            }
            free(pszOutput);
        } else {
            INetwork::SendChatToClient(iRequester, "^cffff", SelectLangMessage(iRequester,
                "^c00ff80Browse finished with no data.^r",
                "^c00ff80Поиск завершён, данных нет.^r"));
        }

        EnterCriticalSection(&_csBrowse);
        _browse.bBusy = FALSE;
        LeaveCriticalSection(&_csBrowse);
        return;
    }

    if (!bBusy) return; /* thread ended without ever setting bReady - bail quietly */

    if (iAttempt >= BROWSE_MAX_POLLS) {
        INetwork::SendChatToClient(iRequester, "^cffff", SelectLangMessage(iRequester,
            "^c00ff80Server browse timed out.^r",
            "^c00ff80Поиск серверов истёк по времени.^r"));
        EnterCriticalSection(&_csBrowse);
        _browse.bBusy = FALSE;
        LeaveCriticalSection(&_csBrowse);
        return;
    }

    CTString strNext;
    strNext.PrintF("BrowseCheckResult(%d);", (int)(iAttempt + 1));
    IScriptScheduler::Schedule(1.0f, strNext);
};

/* ---- Public API ----------------------------------------------------------- */

void PlayersBrowse_Init(void) {
    InitializeCriticalSection(&_csBrowse);

    _bWinHttpLinked = PB_LinkWinHttp();
    if (!_bWinHttpLinked) {
        CPrintF(TRANS("PlayersBrowse: could not link WinHTTP - @browse will report unavailable.\n"));
    }

    _pShell->DeclareSymbol("user void BrowsePublicServers(INDEX);", &BrowsePublicServersFunc);
    _pShell->DeclareSymbol("user void BrowseCheckResult(INDEX);", &BrowseCheckResultFunc);
};