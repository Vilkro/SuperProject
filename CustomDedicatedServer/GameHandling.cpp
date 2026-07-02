/* Copyright (c) 2002-2012 Croteam Ltd.
This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as published by
the Free Software Foundation


This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "StdH.h"

// [Cecil] Classics Patch
#include <Core/Networking/Modules/VotingSystem.h>

// Game state properties
INDEX _iRound = 1;
BOOL _bHadPlayers = FALSE;
BOOL _bRestart = FALSE;

CTimerValue _tvLastLevelEnd(-1i64);
static CTimerValue _tvEmptySince(-1i64);   // 1111 - when the server first went empty

static CTFileName _fnmLastKnownWorld;   // 1111 - default-constructs empty, no need to force it

static CTFileName _fnmLastWorld;          // 1111 - tracks loaded world for coop detection
static BOOL _bSkipNextWorldChange = FALSE;    // 1111 - set before intentional map loads

static CTString strBegScript;
static CTString strEndScript;

// Start new game on a specific level
static BOOL StartGame(const CTFileName &fnmLevel)
{
  // [Cecil] Reset start player indices
  GetGameAPI()->ResetStartProfiles();

  GetGameAPI()->SetNetworkProvider(CGameAPI::NP_SERVER);

  BOOL bGameStarted;

  // [Cecil] Load a saved game
  if (fnmLevel.FileExt() == ".sav") {
    bGameStarted = _pGame->LoadGame(fnmLevel);

  // Start new game
  } else {
    GetGameAPI()->SetCustomLevel(fnmLevel);

    // [Cecil] Pass byte container
    CSesPropsContainer sp;
    _pGame->SetMultiPlayerSession((CSessionProperties &)sp);

    // [Cecil] Start game through the API
    bGameStarted = GetGameAPI()->NewGame(GetGameAPI()->SessionName(), fnmLevel, (CSessionProperties &)sp);
  }

  return bGameStarted;
};

// Begin round on the current map
void RoundBegin(void)
{
  // [Cecil] Select new scripts only if not changing levels mid-game
  const BOOL bLevelChange = (ded_strForceLevelChange != "");

  if (!bLevelChange) {
    // repeat generate script names
    FOREVER {
      strBegScript.PrintF("%s%d_begin.ini", ded_strConfig, _iRound);
      strEndScript.PrintF("%s%d_end.ini", ded_strConfig, _iRound);

      // if start script exists
      if (FileExists(strBegScript)) {
        // stop searching
        break;

      // if start script doesn't exist
      } else {
        // if this is first round
        if (_iRound == 1) {
          // error
          CPutString(LOCALIZE("No scripts present!\n"));
          _bRunning = FALSE;
          return;
        }

        // try again with first round
        _iRound = 1;
      }
    }
  }

  // run start script
  ExecScript(strBegScript);

  // [Cecil] Force new level
  if (bLevelChange) {
    ded_strLevel = ded_strForceLevelChange;
  }

  // start the level specified there
  if (ded_strLevel == "") {
    CPutString(LOCALIZE("ERROR: No next level specified!\n"));
    _bRunning = FALSE;

  } else {
      _bSkipNextWorldChange = TRUE;     // 1111 - server is starting this load deliberately
      if (StartNewMap()) {
          CPutString(LOCALIZE("\nALL OK: Dedicated server is now running!\n"));
          CPutString(LOCALIZE("Use Ctrl+C to shutdown the server.\n"));
          CPutString(LOCALIZE("DO NOT use the 'Close' button, it might leave the port hanging!\n\n"));
      }
  }
};

// End round on the current map
void RoundEnd(BOOL bGameEnd)
{
  // Stop game
  if (bGameEnd) {
    _pGame->StopGame();

  // End current round
  } else {
    CPutString("end of round---------------------------\n");

    ExecScript(strEndScript);
    _iRound++;
  }
};

// Start new map loading
BOOL StartNewMap(void) {
  EnableLoadingHook();

  // [Cecil] Couldn't start the game
  if (!StartGame(ded_strLevel)) {
    CPutString(TRANS("ERROR: Couldn't start a new game!\n"));
    return FALSE;
  }

  _bHadPlayers = FALSE;
  _bRestart = FALSE;

  DisableLoadingHook();
  _tvLastLevelEnd = CTimerValue(-1i64);
  _tvEmptySince = CTimerValue(-1i64);   // 1111

  return TRUE;
};

// Limit current frame rate if neeeded
static void LimitFrameRate(void) {
  // measure passed time for each loop
  static CTimerValue tvLast(-1.0f);

  CTimerValue tvNow = _pTimer->GetHighPrecisionTimer();
  TIME tmCurrentDelta = (tvNow - tvLast).GetSeconds();

  // limit maximum frame rate
  ded_iMaxFPS = ClampDn(ded_iMaxFPS, 1L);
  TIME tmWantedDelta = 1.0f / ded_iMaxFPS;

  if (tmCurrentDelta < tmWantedDelta) {
    Sleep((tmWantedDelta - tmCurrentDelta) * 1000.0f);
  }

  // remember new time
  tvLast = _pTimer->GetHighPrecisionTimer();
};

// [1111] returns TRUE once ded_tmRestartWhenEmptyDelay seconds have passed
// since the room became empty (tracked in _tvEmptySince).
static BOOL EmptyDelayElapsed(void)
{
    if (_tvEmptySince.tv_llValue < 0) {
        _tvEmptySince = _pTimer->GetHighPrecisionTimer();
    }
    return (_pTimer->GetHighPrecisionTimer() - _tvEmptySince).GetSeconds() >= ded_tmRestartWhenEmptyDelay;
}

// [1111] Fires when the engine loads a new world during GameMainLoop (coop level finish).
// Unlike RoundBegin, the map is ALREADY loaded here — we only run scripts.
static void OnCoopLevelChange(const CTFileName& fnmNew)
{
    CPutString("end of round (coop)---------------------------\n");
    ExecScript(strEndScript);
    _iRound++;

    // pick up the next begin/end ini, wrapping to 1 if N doesn't exist
    FOREVER{
      strBegScript.PrintF("%s%d_begin.ini", ded_strConfig, _iRound);
      strEndScript.PrintF("%s%d_end.ini",   ded_strConfig, _iRound);

      if (FileExists(strBegScript)) break;

      if (_iRound == 1) {
        CPutString(LOCALIZE("No scripts present!\n"));
        return;
      }
      _iRound = 1;
    }

    CPutString("begin of round (coop)---------------------------\n");
    ExecScript(strBegScript);           // run begin ini — map is already loaded

    _tvLastLevelEnd = CTimerValue(-1i64);   // prevent IsGameFinished path from double-firing
}

// Main game loop
void DoGame(void)
{
    if (GetGameAPI()->IsGameOn()) {
        _pGame->GameMainLoop();

        // ---- detect coop level transitions - world changed inside GameMainLoop ----  1111
        const CTFileName& fnmCurrent = _pNetwork->ga_World.wo_fnmFileName;
        if (_fnmLastWorld != "" && fnmCurrent != "" && fnmCurrent != _fnmLastWorld) {
            if (_bSkipNextWorldChange) {
                _bSkipNextWorldChange = FALSE;    // intentional load — clear and skip
            }
            else {
                OnCoopLevelChange(fnmCurrent);    // genuine coop transition
            }
        }
        if (fnmCurrent != "") {
            _fnmLastWorld = fnmCurrent;
        }
        // -----------------------------------------------------------------------------

        // if any player is connected
        if (_pGame->GetPlayersCount()) {
            if (!_bHadPlayers) {
                if (_pNetwork->IsPaused()) {
                    _pNetwork->TogglePause();
                }
            }
            _bHadPlayers = TRUE;
            _tvEmptySince = CTimerValue(-1i64);                                   // 1111

            // if no player is connected
        }
        else {
            // never had any player yet - keep paused, same as before
                if (!_bHadPlayers) {
                    if (!_pNetwork->IsPaused()) {
                        _pNetwork->TogglePause();
                    }

                    // [1111] only restart while waiting for the first-ever player
                    // if explicitly enabled - default off, preserves old behavior.
                    if (ded_bRestartWhenPaused && EmptyDelayElapsed()) {
                        _bRestart = TRUE;
                    }

                    // had players, room just emptied
                }
                else {
                    if (EmptyDelayElapsed()) {
                        _bRestart = TRUE;
                    }
            }
        }

    }
    else {
    // just handle broadcast messages
        _pNetwork->GameInactive();
    }

  // [Cecil] Update current vote
    IVotingSystem::UpdateVote();
  // limit current frame rate if needed
    LimitFrameRate();
};