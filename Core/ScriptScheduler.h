/* ScriptScheduler.h   (1111)
 *
 * Minimal delayed-execution queue for shell scripts. Exposes the shell
 * function ScheduleScript(FLOAT fDelay, CTString strScript), which queues a
 * script string to run fDelay seconds later on the main thread, plus
 * CancelScheduled() to wipe the queue. Used to implement self-rescheduling
 * timers in init.ini (e.g. periodic announcements, demo rotation - see
 * DemoManager.cpp's init.ini snippet) without needing native code for every
 * one-off timed behavior. Backed by a fixed-size array (SCHEDULER_MAX_ENTRIES)
 * rather than a dynamic container - scripts are dropped with a console
 * warning if the queue is full, never silently lost.
 */

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
