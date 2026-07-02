/* DemoManager.h   // 1111
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
