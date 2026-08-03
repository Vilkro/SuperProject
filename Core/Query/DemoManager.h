/* DemoManager.h   // 1111
 *
 * Server-side .dem demo recording. Exposes shell functions StartDemoRec(),
 * StopDemoRec(), IsDemoRecording(), GetDemoName() and GetActivePlayerCount()
 * (declared/registered in DemoManager.cpp, not here - this header only
 * exposes the lifecycle hooks). Recording itself is driven entirely from
 * init.ini script (e.g. auto-rotating demos every N seconds via
 * ScheduleScript) - see the init.ini snippet at the bottom of DemoManager.cpp.
 *
 * DROP INTO: Core/Query/DemoManager.h
 */

#ifndef DEMOMANAGER_H
#define DEMOMANAGER_H

#ifdef PRAGMA_ONCE
  #pragma once
#endif

// Call from InitQuery() in Core/Query/QueryManager.cpp (after PlayerDB_Init)
extern void DemoManager_Init(void);

// Call from ClassicsPatch_Shutdown() in Core/Core.cpp
extern void DemoManager_Shutdown(void);

#endif // DEMOMANAGER_H
