Ragtime for Windows
===================

Ragtime is a plain, friendly chat and file-transfer client for Data2G,
the HF data modem by Andrew Rodland, KC2G. https://github.com/baitisj/ragtime

This is a portable build: there is no installer. Unzip the folder anywhere
and run ragtime.exe. To remove Ragtime, delete the folder, the settings
under HKEY_CURRENT_USER\Software\Ragtime and the folder %APPDATA%\Ragtime.

You need data2g-host
--------------------

Ragtime is only a front end. It talks over TCP to a data2g-host that you
run yourself, on this computer or another one on your network; Ragtime
never starts it, never bundles it, and never touches the sound card. It
keys the rig only if you tick Key the rig in Settings > Rig.
Get Data2G from https://github.com/arodland/Data2G and follow its own
instructions to run data2g-host with your radio.

In Ragtime, open Settings > Station and give your callsign and locator, and
the data2g-host address and ports. The defaults (127.0.0.1, KISS port
8100, command port 8300, session data on 8301) match data2g-host's own.

The gearshift's figures
-----------------------

The gearshift (Settings > Gearshift, Ctrl+G) picks a Data2G mode from the
modes' sensitivity thresholds. Nothing of Data2G's is built into Ragtime;
it reads them at run time from Data2G's own file,

    data2g\codes_data\mode_thresholds.json

in your Data2G folder. Ragtime finds the folder if it is %DATA2G_HOME%, or
named Data2G or data2g in your user folder (C:\Users\<you>) or in its src,
git, code, projects, repos or dev folder. Otherwise pick the file with Load
thresholds... in the gearshift, or put a copy at
%APPDATA%\Ragtime\submode_thresholds.json. Data2G running on another
computer is fine: copy its mode_thresholds.json over. Without thresholds
Ragtime still chats; it just sends in the host's own mode.

Where things go
---------------

  Settings          HKEY_CURRENT_USER\Software\Ragtime
  Ragtime's files   %APPDATA%\Ragtime (the files log, thresholds)
  Received files    Documents\Ragtime received (Settings > Station)

Ragtime is MIT licensed: see LICENSE.txt. It includes wxWidgets
(https://wxwidgets.org), under the wxWindows Library Licence.
