Gears of War 2: Hollow - Controller & HUD Fix  (v1.0)
=====================================================

Two separate fixes. Install either one, or both.

CONTROLLER FIX
  When Hollow is added to Steam as a non-Steam game (needed for online co-op), Steam can make
  one controller show up as two: you appear signed in twice on the main menu and a second
  player is created. With this fix the game only ever sees one controller. Steam Input,
  the Steam overlay and online play keep working.
  Note: local split-screen on one PC needs more than one controller, so don't use this fix
  for that.

HUD FIX
  The in-game HUD (weapon and ammo, crosshair, prompts) and subtitles stay at their original
  720p pixel size, so they look tiny at 1440p or 4K. This fix scales them up to match your
  resolution: 2x at 1440p, 3x at 4K. The menus already scale on their own and aren't changed.


EASY INSTALL (installer)
------------------------
Run HollowFixInstaller.exe. It finds your game folder (or press Browse), lets you tick the
fixes you want, and installs them. Run it again any time to change or uninstall.


MANUAL INSTALL (no installer)
-----------------------------
1. Open your Gears of War 2 - Hollow folder, then open the "Binaries" folder inside it.
   This is the folder that contains GoW2Hollow.exe.
       e.g.  C:\Games\Gears of War 2 - Hollow\Binaries

2. Pick ONE of these folders from this zip and copy everything inside it into Binaries:
       1 - Both Fixes
       2 - Controller Fix Only
       3 - HUD Fix Only

3. Start the game the way you normally do (through Steam or the launcher).

Switching later: open Binaries\HollowInputFix.ini in Notepad and set
       ControllerFix=1 or 0
       HUDFix=1 or 0
If you turn the controller fix on in the ini, make sure dinput8.dll is also in Binaries
(it comes in the "Both Fixes" and "Controller Fix Only" folders).

Uninstall: delete these files from Binaries:
       xinput1_3.dll   dinput8.dll   HollowInputFix.ini   HollowInputFix.log


SETTINGS  (Binaries\HollowInputFix.ini)
---------------------------------------
  HUD too big?   Under [HUD], set ReferenceHeight=1080 for a smaller HUD, or set a fixed
                 size with Scale=1.5 (Scale=auto is the default).
  Split-screen?  Set ControllerFix=0.
Each setting has a comment in the ini that explains it.


TROUBLESHOOTING
---------------
- The game writes Binaries\HollowInputFix.log. It shows which fixes turned on and which
  controllers were found. Include it if you report a problem.
- The fixes were made for the current Hollow build (changelist 297571). If Hollow updates
  its game exe, each fix checks the game before changing anything and turns itself off where
  it doesn't match. The game will still run normally, but the fix may stop working until it
  is updated.
- If another mod already uses xinput1_3.dll or dinput8.dll, it conflicts with this one.
  The installer backs those files up and restores them when you uninstall. With a manual
  install, you'd be overwriting them.


HOW IT WORKS (for the curious)
------------------------------
xinput1_3.dll and dinput8.dll are small proxy DLLs that the game loads from its own folder in
place of the Windows ones. They forward everything to the real Windows DLLs, except that:
  - controller reads are filtered so the game sees a single pad, and DirectInput duplicates
    are hidden;
  - the game's HUD canvas is laid out at 720p and scaled up on the GPU.
Nothing is written to the game's own files. Source code is in the "Source" folder.
