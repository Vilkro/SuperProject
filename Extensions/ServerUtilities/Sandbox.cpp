/* Copyright (c) 2022-2026 Dreamy Cecil
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

#include "Sandbox.h"

// Scheduled commands to be executed after the world loads
CStringStack IServerSandbox::astrScheduled;

// Schedule one command
void IServerSandbox::ScheduleCommand(const CTString &strCommand) {
  CPutString(TRANS("Scheduled command for the server:\n"));
  CPrintF("  %s\n", strCommand);

  astrScheduled.Push() = strCommand;
};

// List all scheduled commands in order
void IServerSandbox::ListScheduledCommands(void) {
  if (astrScheduled.Count() == 0) {
    CPutString(TRANS("No commands have been scheduled\n"));
    return;
  }

  CPutString(TRANS("Scheduled commands for the next server start:\n"));

  for (INDEX iCommand = 0; iCommand < astrScheduled.Count(); iCommand++) {
    CPrintF("  %s\n", astrScheduled[iCommand]);
  }
};

// Clear scheduled commands
void IServerSandbox::ClearScheduledCommands(void) {
  CPrintF(TRANS("Cleared %d scheduled commands\n"), astrScheduled.Count());

  astrScheduled.Clear();
};

// Delete an entity from the world
void IServerSandbox::DeleteEntity(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iEntityID = NEXT_ARG(INDEX);

  if (iEntityID < 0) {
    CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
    return;
  }

  // Schedule the command before the game starts
  if (!_pNetwork->IsServer()) {
    CTString strCommand;
    strCommand.PrintF("sutl_DeleteEntity(%d);", iEntityID);

    ScheduleCommand(strCommand);
    return;
  }

  // NEW: sentinel ID triggers bulk removal of gameplay entities instead of a single delete     1111
  if (iEntityID == 999999) {
      DeleteGameplayEntities();
      return;
  }

  CEntity *pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

  // No entity
  if (pen == NULL) {
    CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
    return;
  }
  
  CPrintF(TRANS("Destroyed '%s' entity under ID: %d\n"), pen->GetName(), iEntityID);
  pen->Destroy();
};


// ---------------------------------------------------------------------------------------  1111
// List every entity in the world with its class name - READ ONLY, deletes nothing     1111
void IServerSandbox::ListWorldEntities(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX ctTotal = 0;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        CPrintF("[%s] %s\n", strClass, pen->GetName());
        ctTotal++;
    }

    CPrintF(TRANS("Total entities: %d\n"), ctTotal);
};

// Class names to delete in bulk mode - fill in from sutl_ListWorldEntities() output     1111
static BOOL IsClassToDelete(const CTString& strClass) {
    static const char* astrTargets[] = {
      "Enemy Spawner",  // stops every species from ever spawning, on any map
      "World link",     // level transition - same class name on every stock map
      // "Trigger",

      // Monster template/species classes - confirmed present in your dump
      "Scorpman", "Fish", "BigHead", "Grunt", "ChainsawFreak", "Boneman",
      "Eyeman", "Headman", "Werebull", "Woman", "Gizmo", "Elemental",
      "Guffy", "Walker", "Beast", "Devil", "CannonRotating", "Demon",
      "CannonStatic", "ExotechLarva", "AirElemental", "Summoner",

      // On-screen trigger messages
      // "MessageHolder",
    };

    for (INDEX i = 0; i < ARRAYCOUNT(astrTargets); i++) {
        if (strClass == astrTargets[i]) return TRUE;
    }
    return FALSE;
};

// NEW: sentinel ID triggers bulk removal of gameplay entities instead of a single delete     1111
void IServerSandbox::DeleteGameplayEntities(void) {
    CWorld* pwo = IWorld::GetWorld();
    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX iOff = 0;

    CDynamicContainer<CEntity> cToDelete;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        /*if (strClass == "Trigger" && pen->GetName() != "Trigger Coop marker") {
            cToDelete.Add(pen);
            continue;
        }*/

        if (IsClassToDelete(strClass)) {
            cToDelete.Add(pen);
        }
    }

    INDEX ctDeleted = cToDelete.Count();

    FOREACHINDYNAMICCONTAINER(cToDelete, CEntity, itenDel) {
        // CPrintF("Deleting '%s' [%s]\n", itenDel->GetName(),
        //     itenDel->GetClass()->ec_pdecDLLClass->dec_strName);
        itenDel->Destroy();
    }

    CPrintF(TRANS("Deleted %d entities\n"), ctDeleted);
};

void IServerSandbox::CapBrushHealth(void) {
    CWorld* pwo = IWorld::GetWorld();
    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX iOff = 0;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        if (strClass == "Moving Brush") {
            CLiveEntity* penLive = (CLiveEntity*)pen; // valid: CMovingBrush derives from CLiveEntity

            if (penLive->GetHealth() > 150.0f) {
                penLive->SetHealth(150.0f);   // actually rewrites en_fHealth, used by ReceiveDamage
            }

            CEntityProperty* pep = pen->PropertyForName("Blowup by Damager");

            if (pep == NULL) {
                CPrintF(TRANS("Could not find 'Blowup by Damager' property on '%s'\n"), pen->GetName());
                continue;
            }

            IProperties::SetPropValue(pen, pep, &iOff);

            CEntityProperty* pepTarget = pen->PropertyForName("Health");
            if (pepTarget == NULL) {
                CPrintF(TRANS("Could not find 'Health' property on '%s'\n"), pen->GetName());
                continue;
            }

            void* pTargetValue = NULL;
            IProperties::GetPropValue(pen, pepTarget, &pTargetValue);
            if (pTargetValue == NULL) {
                continue;
            }

            FLOAT fCurrentHP = *(FLOAT*)pTargetValue;
            if (fCurrentHP < 150.0f) {
                continue;
            }

            FLOAT fCapHP = 150.0f;
            IProperties::SetPropValue(pen, pepTarget, &fCapHP);
        }
    }

    //// Schedule the command before the game starts
    //if (!_pNetwork->IsServer()) {
    //    CTString strCommand;
    //    strCommand.PrintF("sutl_CapBrushHealth();");

    //    ScheduleCommand(strCommand);
    //    return;
    //}
};

// Enable shoot-to-activate on every Moving Brush in the world  1111
void IServerSandbox::EnableMoverDamageActivation(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX ctChanged = 0;
    INDEX iOn = 1;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        if (strClass != "Moving Brush") continue;

        // Skip brushes with no Target - those are destructibles, not doors
        CEntityProperty* pepTarget = pen->PropertyForName("Target");
        if (pepTarget != NULL) {
            void* pTargetValue = NULL;
            IProperties::GetPropValue(pen, pepTarget, &pTargetValue);

            if (pTargetValue == NULL || *(CEntity**)pTargetValue == NULL) {
                continue;  // no target, skip
            }
        }

        CEntityProperty* pep = pen->PropertyForName("Move on damage");

        if (pep == NULL) {
            CPrintF(TRANS("Could not find 'Move on damage' property on '%s'\n"), pen->GetName());
            continue;
        }

        if (IProperties::SetPropValue(pen, pep, &iOn)) {
            ctChanged++;
        }
    }

    CPrintF(TRANS("Enabled shoot-to-activate on %d moving brushes with targets\n"), ctChanged);
};

// Enable touch-to-activate on every Moving Brush in the world  1111
void IServerSandbox::EnableTouchActivation(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX ctChanged = 0;
    INDEX iOn = 1;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        if (strClass != "Moving Brush") continue;

        // Skip brushes with no Target - those are destructibles, not doors
        CEntityProperty* pepTarget = pen->PropertyForName("Target");
        if (pepTarget != NULL) {
            void* pTargetValue = NULL;
            IProperties::GetPropValue(pen, pepTarget, &pTargetValue);

            if (pTargetValue == NULL || *(CEntity**)pTargetValue == NULL) {
                continue;  // no target, skip
            }
        }

        CEntityProperty* pep = pen->PropertyForName("Move on touch");

        if (pep == NULL) {
            CPrintF(TRANS("Could not find 'Move on touch' property on '%s'\n"), pen->GetName());
            continue;
        }

        if (IProperties::SetPropValue(pen, pep, &iOn)) {
            ctChanged++;
        }
    }

    CPrintF(TRANS("Enabled touch-to-activate on %d moving brushes with targets\n"), ctChanged);
};

void IServerSandbox::CacheWorldBase(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    CDynamicContainer<CEntity> cWorldBaseCache;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;
        if (strClass == "WorldBase") {
            cWorldBaseCache.Add(pen);
        }
    }

    CPrintF(TRANS("Cached %d WorldBase entities\n"), cWorldBaseCache.Count());
};

// Enable Auto Start on every Trigger entity in the world
void IServerSandbox::EnableTriggerAutoStart(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX ctChanged = 0;
    INDEX iOn = 1;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        if (strClass != "Moving Brush") continue;

        CEntityProperty* pep = pen->PropertyForName("Auto start");

        if (pep == NULL) {
            CPrintF(TRANS("Could not find 'Auto start' property on '%s'\n"), pen->GetName());
            continue;
        }

        if (IProperties::SetPropValue(pen, pep, &iOn)) {
            ctChanged++;
        }
    }

    CPrintF(TRANS("Enabled Auto Start on %d triggers\n"), ctChanged);
};

void IServerSandbox::DeleteCoopMarkers(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    CDynamicContainer<CEntity> cToDelete;

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        if (strClass == "Trigger" && pen->GetName() == "Trigger Coop marker") {
            cToDelete.Add(pen);
        }
    }

    INDEX ctDeleted = cToDelete.Count();

    FOREACHINDYNAMICCONTAINER(cToDelete, CEntity, itenDel) {
        itenDel->Destroy();
    }

    CPrintF(TRANS("Deleted %d coop marker triggers\n"), ctDeleted);
};

void IServerSandbox::MakeCoopMarkersReusable(void) {
    CWorld* pwo = IWorld::GetWorld();

    if (pwo == NULL) {
        CPutString(TRANS("No world loaded\n"));
        return;
    }

    INDEX ctChanged = 0;
    CTString strMessage = "^f4^c1FFFB7The respawn point has been updated^r";
    INDEX iMaxTrigs = 9999;

    ScheduleCommand("ent_bAnnounceCoopMarkers = 1;");

    FOREACHINDYNAMICCONTAINER(pwo->wo_cenEntities, CEntity, iten) {
        CEntity* pen = &*iten;
        const CTString& strClass = pen->GetClass()->ec_pdecDLLClass->dec_strName;

        if (strClass != "Trigger" || pen->GetName() != "Trigger Coop marker" && pen->GetName() != "Coop Marker Trigger") continue;

        CEntityProperty* pepMsg = pen->PropertyForName("Message");
        CEntityProperty* pepMax = pen->PropertyForName("Max trigs");

        BOOL bMsgOk = (pepMsg != NULL) ? IProperties::SetPropValue(pen, pepMsg, &strMessage) : FALSE;
        BOOL bMaxOk = (pepMax != NULL) ? IProperties::SetPropValue(pen, pepMax, &iMaxTrigs) : FALSE;

        if (bMsgOk && bMaxOk) ctChanged++;
    }

    CPrintF(TRANS("Made %d coop markers reusable\n"), ctChanged);
};
// ---------------------------------------------------------------------------------------

// Initialize/reinitialize an entity
void IServerSandbox::InitEntity(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iEntityID = NEXT_ARG(INDEX);

  if (iEntityID < 0) {
    CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
    return;
  }

  // Schedule the command before the game starts
  if (!_pNetwork->IsServer()) {
    CTString strCommand;
    strCommand.PrintF("sutl_InitEntity(%d);", iEntityID);

    ScheduleCommand(strCommand);
    return;
  }

  CEntity *pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

  // No entity
  if (pen == NULL) {
    CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
    return;
  }

  // Reinitialize if some render type has already been set
  if (pen->GetRenderType() == CEntity::RT_NONE) {
    CPrintF(TRANS("Initialized '%s' entity under ID: %d\n"), pen->GetName(), iEntityID);
    pen->Initialize();

  } else {
    CPrintF(TRANS("Reinitialized '%s' entity under ID: %d\n"), pen->GetName(), iEntityID);
    pen->Reinitialize();
  }
};

// Set new absolute position of an entity
void IServerSandbox::SetEntityPosition(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iEntityID = NEXT_ARG(INDEX);
  FLOAT fX = NEXT_ARG(FLOAT);
  FLOAT fY = NEXT_ARG(FLOAT);
  FLOAT fZ = NEXT_ARG(FLOAT);

  if (iEntityID < 0) {
    CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
    return;
  }

  // Schedule the command before the game starts
  if (!_pNetwork->IsServer()) {
    CTString strCommand;
    strCommand.PrintF("sutl_SetEntityPosition(%d, %f, %f, %f);", iEntityID, fX, fY, fZ);

    ScheduleCommand(strCommand);
    return;
  }

  CEntity *pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

  // No entity
  if (pen == NULL) {
    CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
    return;
  }

  CPlacement3D plEntity = pen->GetPlacement();
  plEntity.pl_PositionVector = FLOAT3D(fX, fY, fZ);

  pen->Teleport(plEntity, FALSE);
};

// Set new absolute rotation of an entity
void IServerSandbox::SetEntityRotation(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iEntityID = NEXT_ARG(INDEX);
  FLOAT fH = NEXT_ARG(FLOAT);
  FLOAT fP = NEXT_ARG(FLOAT);
  FLOAT fB = NEXT_ARG(FLOAT);

  if (iEntityID < 0) {
    CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
    return;
  }

  // Schedule the command before the game starts
  if (!_pNetwork->IsServer()) {
    CTString strCommand;
    strCommand.PrintF("sutl_SetEntityRotation(%d, %f, %f, %f);", iEntityID, fH, fP, fB);

    ScheduleCommand(strCommand);
    return;
  }

  CEntity *pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

  // No entity
  if (pen == NULL) {
    CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
    return;
  }

  CPlacement3D plEntity = pen->GetPlacement();
  plEntity.pl_OrientationAngle = ANGLE3D(fH, fP, fB);

  pen->Teleport(plEntity, FALSE);
};

// Set new value to some property by its name of an entity
void IServerSandbox::SetEntityProperty(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iEntityID = NEXT_ARG(INDEX);
  const CTString &strProperty = *NEXT_ARG(CTString *);
  CTString &strValue = *NEXT_ARG(CTString *);

  if (iEntityID < 0) {
    CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
    return;
  }

  // Schedule the command before the game starts
  if (!_pNetwork->IsServer()) {
    CTString strCommand;
    strCommand.PrintF("sutl_SetEntityProperty(%d, \"%s\", \"%s\");", iEntityID, strProperty, strValue);

    ScheduleCommand(strCommand);
    return;
  }
  
  CEntity *pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

  // No entity
  if (pen == NULL) {
    CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
    return;
  }

  CEntityProperty *pep = pen->PropertyForName(strProperty);

  // No property
  if (pep == NULL) {
    CPrintF(TRANS("Could not find entity property with the name '%s' in %s\n"), strProperty, pen->GetClass()->ec_pdecDLLClass->dec_strName);
    return;
  }

  INDEX iPropType = IProperties::ConvertType(pep->ep_eptType);
  BOOL bPropertySet = FALSE;

  switch (iPropType)
  {
    case CEntityProperty::EPT_INDEX: {
      INDEX iIndex;
      strValue.ScanF("%d", &iIndex);

      bPropertySet = IProperties::SetPropValue(pen, pep, &iIndex);
    } break;

    case CEntityProperty::EPT_FLOAT: {
      FLOAT fFloat;
      strValue.ScanF("%g", &fFloat);

      bPropertySet = IProperties::SetPropValue(pen, pep, &fFloat);
    } break;

    case CEntityProperty::EPT_STRING: {
      bPropertySet = IProperties::SetPropValue(pen, pep, &strValue);
    } break;

    case CEntityProperty::EPT_ENTITYPTR: {
      INDEX iEntityID;
      strValue.ScanF("%d", &iEntityID);

      CEntity *penSet = NULL;

      if (iEntityID >= 0) {
        penSet = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);
      }

      bPropertySet = IProperties::SetPropValue(pen, pep, &penSet);
    } break;

    case CEntityProperty::EPT_FLOAT3D: {
      FLOAT3D vVector;
      strValue.ScanF("%g,%g,%g", &vVector(1), &vVector(2), &vVector(3));

      bPropertySet = IProperties::SetPropValue(pen, pep, &vVector);
    } break;
  }

  // Couldn't set new value
  if (!bPropertySet) {
      CPrintF(TRANS("Could not set '%s' value to '%s' property\n"), strValue, strProperty);
  }
  else {
      CPrintF(TRANS("Set '%s' property to '%s' on '%s' (ID %d)\n"), strProperty, strValue, pen->GetName(), iEntityID);
  }
};

// Read the current value of a property by name from an entity
void IServerSandbox::GetEntityProperty(SHELL_FUNC_ARGS) {
    BEGIN_SHELL_FUNC;
    INDEX iEntityID = NEXT_ARG(INDEX);
    const CTString& strProperty = *NEXT_ARG(CTString*);

    if (iEntityID < 0) {
        CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
        return;
    }

    if (!_pNetwork->IsServer()) {
        CPrintF(TRANS("Server hasn't started yet, no world loaded\n"));
        return;
    }

    CEntity* pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

    if (pen == NULL) {
        CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
        return;
    }

    CEntityProperty* pep = pen->PropertyForName(strProperty);

    if (pep == NULL) {
        CPrintF(TRANS("Could not find entity property with the name '%s' in %s\n"), strProperty, pen->GetClass()->ec_pdecDLLClass->dec_strName);
        return;
    }

    void* pValue = NULL;
    IProperties::GetPropValue(pen, pep, &pValue);

    INDEX iPropType = IProperties::ConvertType(pep->ep_eptType);

    switch (iPropType)
    {
    case CEntityProperty::EPT_INDEX:
        CPrintF("%s = %d\n", strProperty, *(INDEX*)pValue);
        break;

    case CEntityProperty::EPT_FLOAT:
        CPrintF("%s = %g\n", strProperty, *(FLOAT*)pValue);
        break;

    case CEntityProperty::EPT_STRING:
        CPrintF("%s = \"%s\"\n", strProperty, *(CTString*)pValue);
        break;

    case CEntityProperty::EPT_ENTITYPTR: {
        CEntity* penVal = *(CEntity**)pValue;
        CPrintF("%s = %s\n", strProperty, penVal != NULL ? penVal->GetName() : CTString("<none>"));
    } break;

    case CEntityProperty::EPT_FLOAT3D: {
        FLOAT3D& vVal = *(FLOAT3D*)pValue;
        CPrintF("%s = %g,%g,%g\n", strProperty, vVal(1), vVal(2), vVal(3));
    } break;
    }
};

// Parent an entity to another entity
void IServerSandbox::ParentEntity(SHELL_FUNC_ARGS) {
  BEGIN_SHELL_FUNC;
  INDEX iEntityID = NEXT_ARG(INDEX);
  INDEX iParentEntityID = NEXT_ARG(INDEX);

  if (iEntityID < 0) {
    CPrintF(TRANS("Invalid entity ID: %d\n"), iEntityID);
    return;
  }

  // Schedule the command before the game starts
  if (!_pNetwork->IsServer()) {
    CTString strCommand;
    strCommand.PrintF("sutl_ParentEntity(%d, %d);", iEntityID, iParentEntityID);

    ScheduleCommand(strCommand);
    return;
  }

  CEntity *pen = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iEntityID);

  // No entity
  if (pen == NULL) {
    CPrintF(TRANS("Could not find entity under ID: %d\n"), iEntityID);
    return;
  }

  // Unparent
  if (iParentEntityID < 0) {
    pen->SetParent(NULL);
    return;
  }

  // Parent to some entity
  CEntity *penParent = IWorld::FindEntityByID(IWorld::GetWorld(), (ULONG)iParentEntityID);
  pen->SetParent(penParent);
};
