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

class IServerSandbox {
  public:
    // Scheduled commands to be executed after the world loads
    static CStringStack astrScheduled;

  public:
    // Schedule one command
    static void ScheduleCommand(const CTString &strCommand);

    // List all scheduled commands in order
    static void ListScheduledCommands(void);
    
    // Clear scheduled commands
    static void ClearScheduledCommands(void);

    // Delete an entity from the world
    static void DeleteEntity(SHELL_FUNC_ARGS);

    // Bulk-delete every entity whose class is on the hardcoded IsClassToDelete()
    // list (spawners, level links, monster species, ...) - read-only, no args;
    // also reachable via DeleteEntity(999999) as a sentinel ID.               (1111)
    static void DeleteGameplayEntities(void);   //  1111

    // Print every entity currently in the world with its class name. Read-only,
    // deletes nothing - use this to find the exact class names to add to
    // DeleteGameplayEntities()'s target list before running it.              (1111)
    static void ListWorldEntities(void);    //  1111

    // Force "Move on damage" (shoot-to-activate) ON for every Moving Brush that
    // has a Target set (brushes with no Target are treated as destructibles,
    // not doors, and are skipped).                                          (1111)
    static void EnableMoverDamageActivation(void);    //  1111

    // Force "Move on touch" ON for every Moving Brush that has a Target set,
    // same Target-less-skip rule as EnableMoverDamageActivation().          (1111)
    static void EnableTouchActivation(void);    //  1111

    // Collect every WorldBase entity into a local container and report the
    // count. Currently just a diagnostic - the cache itself isn't persisted
    // past this call.                                                       (1111)
    static void CacheWorldBase(void);   //  1111

    // Force "Auto start" ON for every Moving Brush's Trigger entity, so
    // triggers fire immediately on world load without needing to be touched. (1111)
    static void EnableTriggerAutoStart(void);    //  1111

    // Delete every Trigger entity named "Trigger Coop marker" from the world. (1111)
    static void DeleteCoopMarkers(void);    //  1111

    // Rewrite every coop-marker Trigger's Message/Max trigs properties so it
    // can be triggered repeatedly (instead of once) and shows a respawn-point
    // update message; also turns on ent_bAnnounceCoopMarkers.               (1111)
    static void MakeCoopMarkersReusable(void);    //  1111

    // Clamp a Moving Brush's health down to 150 if it's currently higher, and
    // read back its "Blowup by Damager" / "Health" properties to do so.      (1111)
    static void CapBrushHealth(void);    //  1111

    // Initialize/reinitialize an entity
    static void InitEntity(SHELL_FUNC_ARGS);

    // Set new absolute position of an entity
    static void SetEntityPosition(SHELL_FUNC_ARGS);

    // Set new absolute rotation of an entity
    static void SetEntityRotation(SHELL_FUNC_ARGS);

    // Set new value to some property by its name of an entity
    static void SetEntityProperty(SHELL_FUNC_ARGS);

    // Read and print the current value of a named property on an entity
    // (counterpart to SetEntityProperty - same lookup, but read-only).       (1111)
    static void GetEntityProperty(SHELL_FUNC_ARGS); //  1111

    // Parent an entity to another entity
    static void ParentEntity(SHELL_FUNC_ARGS);
};
