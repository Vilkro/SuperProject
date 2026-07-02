#ifndef SCRIPTSCHEDULER_H
#define SCRIPTSCHEDULER_H

#ifdef PRAGMA_ONCE
#pragma once
#endif

// Maximum simultaneously pending scheduled scripts
#define SCHEDULER_MAX_ENTRIES 64

struct SScheduledScript {
	BOOL     bActive;
	double   dFireTime;   // absolute wall-clock time in seconds
	CTString strScript;
};

namespace IScriptScheduler {
	void Schedule(FLOAT fDelay, const CTString& strScript);

	// Called from CoreTimerHandler::OnTick() - checks and fires due scripts
	void Tick(void);

	// Cancels all pending scripts
	void CancelAll(void);
};

#endif#pragma once
