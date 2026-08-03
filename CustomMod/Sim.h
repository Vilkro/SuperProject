// Sim.h — Scripted input subsystem for offline rocket-jump research.
// Local, single-player only. See design doc §7 "Fairness / scope boundary".
//
// Phase 1 scope: input injection only. The record-through demo capture
// (§3 steps 2-3) and the real action-script format (§4) are separate,
// later pieces of work — this module intentionally stays minimal.

#ifndef SIM_SCRIPTED_INPUT_H
#define SIM_SCRIPTED_INPUT_H

#include <Engine/Network/NetworkMessage.h> // CPlayerAction
#include <Engine/Base/Shell.h>
#include <Engine/Base/Console.h>
#include <Engine/Templates/DynamicArray.h>
#include <Engine/Templates/DynamicArray.cpp>

// Master switch. Only takes effect in a local, non-networked session —
// see the guard in the CreateAction patch. Declared "persistent user"
// like the existing dem_* cvars in Network.cpp so it can be flipped from
// the console.
extern BOOL sim_bScriptedInput;

// Path to the script file to (re)load. Set this, then set
// sim_bReloadScript = TRUE; the next call to Sim_GetNextAction() will
// load it and clear the trigger.
extern CTString sim_strScriptFile;
extern BOOL sim_bReloadScript;

// Set to TRUE once the loaded script is exhausted. A later batch/optimizer
// driver (Phase 5) can poll this to know when to stop and move to the
// next candidate.
extern BOOL sim_bScriptComplete;

// Registers the sim_* shell symbols. Call once at startup, next to the
// existing dem_fSyncRate / dem_fRealTimeFactor registration in
// Network.cpp (Network.cpp:753-754).
void Sim_RegisterSymbols(void);

// Loads a scripted action sequence from disk. Deliberately minimal,
// flat, one-line-per-tick format for Phase 1 bring-up/testing only —
// NOT the final §4 format (tick ranges, named events like fire_rocket,
// Python-side expansion). Swap this out when §4 is tackled.
//
// File format, one line per tick, whitespace-separated:
//   tx ty tz  rx ry rz  vrx vry vrz  [buttons]
// tx/ty/tz    -> pa_vTranslation(1..3)   (translation(2) is the jump axis)
// rx/ry/rz    -> pa_aRotation(1..3)
// vrx/vry/vrz -> pa_aViewRotation(1..3)
// buttons     -> pa_ulButtons, decimal or 0x-prefixed hex (optional,
//                defaults to 0 — button bit layout is still open item
//                §6 "pa_ulButtons bit layout", not needed for the
//                Phase 1 movement-only verification test)
// Blank lines and lines starting with '#' are ignored.
BOOL Sim_LoadActionScript(const CTFileName &fnScript);

// Resets the internal tick cursor to 0 and clears sim_bScriptComplete.
// Called automatically by Sim_LoadActionScript(); expose separately too
// so a future batch loop can re-run the same script without reloading
// from disk.
void Sim_ResetScriptedInput(void);

// Called from CControls::CreateAction when sim_bScriptedInput is TRUE,
// in place of the normal GetAxisValue()/DoButtonActions() calls. Fills
// paAction from the next entry of the loaded script and advances the
// internal tick cursor. If the script is exhausted, fills paAction with
// a neutral/zeroed action (no movement, no buttons) and sets
// sim_bScriptComplete = TRUE. Also services a pending
// sim_bReloadScript request before pulling the tick.
void Sim_GetNextAction(CPlayerAction &paAction);

#endif // SIM_SCRIPTED_INPUT_H
