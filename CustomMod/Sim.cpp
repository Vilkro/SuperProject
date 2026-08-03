// Sim.cpp — see Sim.h for overview and file-format notes.

#include "StdH.h"
#include "Sim.h"
#include <Engine/Streams/FileStream.h>
#include <stdlib.h>

BOOL sim_bScriptedInput  = FALSE;
CTString sim_strScriptFile = CTString("");
BOOL sim_bReloadScript   = FALSE;
BOOL sim_bScriptComplete = FALSE;

static CDynamicArray<CPlayerAction> sim_aScript;
static INDEX sim_iCursor = 0;

void Sim_RegisterSymbols(void)
{
  _pShell->DeclareSymbol("persistent user BOOL sim_bScriptedInput;",
                          &sim_bScriptedInput);
  _pShell->DeclareSymbol("persistent user CTString sim_strScriptFile;",
                          &sim_strScriptFile);
  _pShell->DeclareSymbol("persistent user BOOL sim_bReloadScript;",
                          &sim_bReloadScript);
  _pShell->DeclareSymbol("user BOOL sim_bScriptComplete;",
                          &sim_bScriptComplete);
}

void Sim_ResetScriptedInput(void)
{
  sim_iCursor = 0;
  sim_bScriptComplete = FALSE;
}

static void ParseButtons(const CTString &strTok, ULONG &ulOut)
{
  if (strTok.Length() == 0) {
    ulOut = 0;
    return;
  }
  if (strTok.HasPrefix("0x") || strTok.HasPrefix("0X")) {
    ulOut = strtoul(((const char *) strTok) + 2, NULL, 16);
  } else {
    ulOut = strtoul((const char *) strTok, NULL, 10);
  }
}

BOOL Sim_LoadActionScript(const CTFileName &fnScript)
{
  sim_aScript.Clear();
  Sim_ResetScriptedInput();

  try {
    CTFileStream strm;
    strm.Open_t(fnScript);

    CTString strLine;
    INDEX iLineNo = 0;

    while (!strm.AtEOF()) {
      strm.GetLine_t(strLine);
      iLineNo++;
      strLine.TrimSpacesLeft();

      if (strLine.Length() == 0 || strLine[0] == '#') {
        continue;
      }

      CPlayerAction pa;
      memset(&pa, 0, sizeof(pa));

      FLOAT afVals[9];
      CTString strButtons("");

      // NOTE: adjust to your actual CTString::ScanF (or equivalent)
      // signature — this assumes sscanf-style % specifiers over the
      // line buffer. If your CTString has no ScanF, swap in a manual
      // tokenizer or sscanf((const char*)strLine, ...).
      INDEX ctScanned = strLine.ScanF(
        "%f %f %f %f %f %f %f %f %f %s",
        &afVals[0], &afVals[1], &afVals[2],
        &afVals[3], &afVals[4], &afVals[5],
        &afVals[6], &afVals[7], &afVals[8],
        &strButtons);

      if (ctScanned < 9) {
        WarningMessage("Sim_LoadActionScript: skipping malformed line %d",
                        (int) iLineNo);
        continue;
      }

      pa.pa_vTranslation  = FLOAT3D(afVals[0], afVals[1], afVals[2]);
      pa.pa_aRotation     = ANGLE3D(afVals[3], afVals[4], afVals[5]);
      pa.pa_aViewRotation = ANGLE3D(afVals[6], afVals[7], afVals[8]);

      ULONG ulButtons = 0;
      if (ctScanned >= 10) {
        ParseButtons(strButtons, ulButtons);
      }
      pa.pa_ulButtons = ulButtons;

      sim_aScript.Add() = pa;
    }

    strm.Close();
  } catch (char *strError) {
    WarningMessage("Sim_LoadActionScript: %s", strError);
    return FALSE;
  }

  CPrintF("Sim_LoadActionScript: loaded %d ticks from %s\n",
          (int) sim_aScript.Count(), (const char *) fnScript);

  return sim_aScript.Count() > 0;
}

void Sim_GetNextAction(CPlayerAction &paAction)
{
  if (sim_bReloadScript) {
    Sim_LoadActionScript(CTFileName(sim_strScriptFile));
    sim_bReloadScript = FALSE;
  }

  if (sim_iCursor < sim_aScript.Count()) {
    paAction = sim_aScript[sim_iCursor];
    sim_iCursor++;
  } else {
    memset(&paAction, 0, sizeof(paAction));
    if (!sim_bScriptComplete && sim_aScript.Count() > 0) {
      CPrintF("Sim_GetNextAction: script complete at tick %d\n",
              (int) sim_iCursor);
    }
    sim_bScriptComplete = TRUE;
  }
}
