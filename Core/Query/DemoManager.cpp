/* DemoManager.cpp  -  Auto demo recording for dedicated servers   // 1111
 *
 * DROP INTO:  Core/Query/DemoManager.cpp
 * ADD TO:     Core.vcxproj  (ClCompile, same section as GeoIP.cpp / PlayerDB.cpp)
 * INCLUDE FROM: Core/Query/QueryManager.cpp  InitQuery()  ->  DemoManager_Init();
 *               Core/Core.cpp  ClassicsPatch_Shutdown()   ->  DemoManager_Shutdown();
 *
 * Shell symbols registered:
 *
 *   void StartDemoRec(CTString strName)
 *       Begins recording to  Demos\<strName>.dem
 *       Safe to call if already recording - silently no-ops.
 *       Server-only (no-op on clients).
 *
 *   void StopDemoRec(void)
 *       Stops any in-progress demo recording.
 *       Server-only.
 *
 *   INDEX IsDemoRecording(void)
 *       Returns 1 while recording, 0 otherwise.
 *
 *   CTString GetDemoName(void)
 *       Returns the filename of the demo currently being recorded,
 *       or "" if not recording.
 *
 * Typical init.ini usage (2-minute rotating demos while server is active):
 *
 *   // -- paste this block at the bottom of init.ini --
 *
 *   cmd_cmdOnJoin = cmd_cmdOnJoin
 *     + "if(IsDemoRecording() == 0){"
 *         + "StartDemoRec(\"auto_\" + (CTString)(INDEX)_pTimer->GetHighPrecisionTimer().GetSeconds());"
 *     + "}\n";
 *
 *   // Self-rescheduling every 120 s:
 *   ScheduleScript(120.0, "StopDemoRec(); ScheduleScript(1.0, \"StartAutoDemo();\");");
 *
 *   // -- see init.ini snippet at bottom of this file for the cleaner version --
 */

#include "StdH.h"

#include "Query/DemoManager.h"

#include "Networking/MessageProcessing.h"
#include "Networking/NetworkFunctions.h"
#include "Networking/Modules.h"
#include <time.h>
#include <string.h>

// ─── state ────────────────────────────────────────────────────────────────────

static char _szCurrentDemoName[256] = "";   // empty when not recording

// ─── helpers ─────────────────────────────────────────────────────────────────

// Build a timestamped filename:  auto_YYYYMMDD_HHMMSS_<mapshort>
static void BuildAutoName(char *szOut, int nOut) {
    time_t t = time(NULL);
    struct tm *tm_now = localtime(&t);

    // Shorten map filename: take only the file stem (no directory, no extension)
    const char *szSession = "";
    if (_pNetwork && _pNetwork->IsServer()) {
        szSession = _pNetwork->ga_strSessionName.Undecorated();
        const char *p = strrchr(szSession, '\\');
        if (!p) p = strrchr(szSession, '/');
        if (p) szSession = p + 1;
        // Strip extension
        static char szSessionShort[64];
        strncpy(szSessionShort, szSession, 63);
        szSessionShort[63] = '\0';
        char *pDot = strrchr(szSessionShort, '.');
        if (pDot) *pDot = '\0';
        // Replace spaces with underscores
        for (char* pChar = szSessionShort; *pChar; ++pChar) {
            if (*pChar == ' ')
                *pChar = '_';
        }
        szSession = szSessionShort;
    }

    _snprintf(szOut, nOut - 1,
        "%04d%02d%02d_%02d%02d%02d_%s",
        tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
        tm_now->tm_hour,        tm_now->tm_min,      tm_now->tm_sec,
        szSession[0] ? szSession : "srv");
    szOut[nOut - 1] = '\0';
}

// Count currently active player slots across all sessions.
// Uses the same pattern as GeoIP.cpp (srv_aplbPlayers / IsActive()).
static INDEX CountActivePlayers(void)
{
    if (_pNetwork == NULL || !_pNetwork->IsServer()) return 0;

    INDEX iCount = 0;
    CServer& srv = _pNetwork->ga_srvServer;

    for (INDEX i = 0; i < srv.srv_aplbPlayers.Count(); i++) {
        if (srv.srv_aplbPlayers[i].IsActive()) iCount++;
    }
    return iCount;
}

// ─── shell functions ──────────────────────────────────────────────────────────

// void StartDemoRec(CTString strName)
static void StartDemoRecFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    const CTString& strDir = *NEXT_ARG(CTString*);
    const CTString& strName = *NEXT_ARG(CTString*);

    if (!_pNetwork || !_pNetwork->IsServer()) return;
    if (_pNetwork->ga_bDemoRec) return;  // already recording

    // Build full path under Demos
    CTString strPath;
    if (strName == "" || strName == CTString("auto")) {
        char szAuto[256];
        BuildAutoName(szAuto, sizeof(szAuto));
        strPath.PrintF("Demos\\%s\\%s.dem", strDir, szAuto);
        strncpy(_szCurrentDemoName, szAuto, sizeof(_szCurrentDemoName) - 1);
    } else {
        strPath.PrintF("Demos\\%s\\%s.dem", strDir, strName.str_String);
        strncpy(_szCurrentDemoName, strName.str_String, sizeof(_szCurrentDemoName) - 1);
    }
    _szCurrentDemoName[sizeof(_szCurrentDemoName) - 1] = '\0';

    try {
        _pNetwork->StartDemoRec_t(CTFileName(strPath));
        CPrintF("[DemoManager] Recording started: %s\n", strPath.str_String);
    } catch (char *strErr) {
        CPrintF("[DemoManager] StartDemoRec failed: %s\n", strErr);
        memset(_szCurrentDemoName, 0, sizeof(_szCurrentDemoName));
    }
}

// void StopDemoRec(void)
static void StopDemoRecFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;

    if (!_pNetwork || !_pNetwork->IsServer()) return;
    if (!_pNetwork->ga_bDemoRec) return;

    _pNetwork->StopDemoRec();
    CPrintF("[DemoManager] Recording stopped: %s\n", _szCurrentDemoName);
    memset(_szCurrentDemoName, 0, sizeof(_szCurrentDemoName));
}

// INDEX IsDemoRecording(void)
static INDEX IsDemoRecordingFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    if (!_pNetwork) return 0;
    return _pNetwork->ga_bDemoRec ? 1 : 0;
}

// CTString GetDemoName(void)
static CTString GetDemoNameFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    return CTString(_szCurrentDemoName);
}

// INDEX GetActivePlayerCount(void)
static INDEX GetActivePlayerCountFunc(SHELL_FUNC_ARGS)
{
    BEGIN_SHELL_FUNC;
    return CountActivePlayers();
}

// ─── lifecycle ────────────────────────────────────────────────────────────────

void DemoManager_Init(void) {
    memset(_szCurrentDemoName, 0, sizeof(_szCurrentDemoName));

    _pShell->DeclareSymbol(
        "user void StartDemoRec(CTString, CTString);",
        &StartDemoRecFunc);                                    /* 1111 */

    _pShell->DeclareSymbol(
        "user void StopDemoRec(void);",
        &StopDemoRecFunc);                                     /* 1111 */

    _pShell->DeclareSymbol(
        "user INDEX IsDemoRecording(void);",
        &IsDemoRecordingFunc);                                 /* 1111 */

    _pShell->DeclareSymbol(
        "user CTString GetDemoName(void);",
        &GetDemoNameFunc);                                     /* 1111 */
    
    _pShell->DeclareSymbol(
        "user INDEX GetActivePlayerCount(void);", 
        &GetActivePlayerCountFunc);                            /* 1111 */

    CPutString("[DemoManager] Initialized.\n");
}

void DemoManager_Shutdown(void) {
    // Stop any in-progress recording cleanly
    if (_pNetwork && _pNetwork->ga_bDemoRec) {
        _pNetwork->StopDemoRec();
        CPutString("[DemoManager] Recording stopped on shutdown.\n");
    }
    CPutString("[DemoManager] Shutdown.\n");
}

/* ─── init.ini snippet ───────────────────────────────────────────────────────
 *
 * Paste this at the END of your init.ini, after all other setup.
 * Requires DemoManager_Init() to have been called (i.e. this .cpp compiled in).
 *
 * The pattern: a self-rescheduling ScheduleScript that rotates demos every
 * 120 seconds whenever the server is active (has at least one session).
 *
 * ─────────────────────────────────────────────────────────────────────────────

// Auto demo recording — rotates every 120 seconds
// Start initial recording when first player joins
cmd_cmdOnJoin = cmd_cmdOnJoin
  + "if(IsDemoRecording() == 0){"
      + "StartDemoRec(\"auto\");"
      + "ScheduleScript(120.0, \"RotateDemo();\");"
  + "}\n";

// Self-rescheduling rotate function
// (CTString concatenation in shell: build the script body as a literal)
ScheduleScript(0.0,
  "user void RotateDemo(void) = {"
    "StopDemoRec();"
    "StartDemoRec(\"auto\");"
    "ScheduleScript(120.0, \"RotateDemo();\");"
  "};"
);

 * ─────────────────────────────────────────────────────────────────────────────
 *
 * NOTE: The SE1 shell does NOT support defining void functions via script.
 * Use the simpler inline reschedule approach instead (shown in init.ini below).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 */
