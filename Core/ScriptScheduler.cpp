#include "StdH.h"
#include "ScriptScheduler.h"

static SScheduledScript _aScheduled[SCHEDULER_MAX_ENTRIES];
static BOOL _bInitialized = FALSE;

static void EnsureInit(void) {
    if (_bInitialized) return;
    for (INDEX i = 0; i < SCHEDULER_MAX_ENTRIES; i++) {
        _aScheduled[i].bActive = FALSE;
    }
    _bInitialized = TRUE;
}

// ─── shell functions ──────────────────────────────────────────────────────────

// ScheduleScript(FLOAT fDelay, CTString strScript)
// Queues strScript to run after fDelay seconds
void ScheduleScriptFunc(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    FLOAT            fDelay = NEXT_ARG(FLOAT);
    const CTString& strScript = *NEXT_ARG(CTString*);

    EnsureInit();

    double dFireTime = _pTimer->GetHighPrecisionTimer().GetSeconds() + (double)fDelay;

    for (INDEX i = 0; i < SCHEDULER_MAX_ENTRIES; i++) {
        if (_aScheduled[i].bActive) continue;

        _aScheduled[i].bActive = TRUE;
        _aScheduled[i].dFireTime = dFireTime;
        _aScheduled[i].strScript = strScript;
        return;
    }

    CPutString("[Scheduler] Warning: no free slots, script dropped.\n");
}

// CancelScheduled()
// Cancels all pending scheduled scripts
void CancelScheduledFunc(SHELL_FUNC_ARGS) {
    IScriptScheduler::CancelAll();
}

// ─── IScriptScheduler ─────────────────────────────────────────────────────────

void IScriptScheduler::Schedule(FLOAT fDelay, const CTString& strScript) {
    EnsureInit();

    double dFireTime = _pTimer->GetHighPrecisionTimer().GetSeconds() + (double)fDelay;

    for (INDEX i = 0; i < SCHEDULER_MAX_ENTRIES; i++) {
        if (_aScheduled[i].bActive) continue;

        _aScheduled[i].bActive = TRUE;
        _aScheduled[i].dFireTime = dFireTime;
        _aScheduled[i].strScript = strScript;
        return;
    }

    CPutString("[Scheduler] Warning: no free slots, script dropped.\n");
}

void IScriptScheduler::Tick(void) {
    if (!_bInitialized) return;

    double dNow = _pTimer->GetHighPrecisionTimer().GetSeconds();

    for (INDEX i = 0; i < SCHEDULER_MAX_ENTRIES; i++) {
        if (!_aScheduled[i].bActive) continue;
        if (_aScheduled[i].dFireTime > dNow) continue;

        // Deactivate before executing so the script can safely reschedule itself
        CTString strScript = _aScheduled[i].strScript;
        _aScheduled[i].bActive = FALSE;
        _aScheduled[i].strScript = "";

        _pShell->Execute(strScript + "\n");
    }
}

void IScriptScheduler::CancelAll(void) {
    EnsureInit();
    for (INDEX i = 0; i < SCHEDULER_MAX_ENTRIES; i++) {
        _aScheduled[i].bActive = FALSE;
        _aScheduled[i].strScript = "";
    }
}