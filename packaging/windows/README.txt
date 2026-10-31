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
never starts it, never bundles it, and never plays to the sound card. Its
Ear (View > Ear) only records, alongside data2g-host, from the input
data2g-host records from (or one you pick, or the default recording
device), in Windows' shared mode. It keys the rig only if you tick Key
the rig in Settings > Rig.
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

Pictures
--------

The picture button beside the floppy disk sends a picture: JPEG, PNG,
GIF, BMP, TIFF, WebP, HEIC or JPEG 2000 from disk, shrunk to the size you
pick and coded as JPEG 2000 to fit the airtime you allow. Pictures come
from the air only as JPEG 2000. The ones you send are kept in
%APPDATA%\Ragtime\sent-pictures.

Voice
-----

The microphone button beside the picture button opens the microphone
deck: hold Push to talk (or the space bar) to record a voice message for
the net or the station you're in a session with, and let go to send it.
It needs sotto.dll beside ragtime.exe. The radio's sound card is never
offered as a microphone or speaker. The clips you send are kept in
%APPDATA%\Ragtime\sent-voice.

Ragtime is MIT licensed: see LICENSE.txt. It includes wxWidgets
(https://wxwidgets.org, the wxWindows Library Licence), Hamlib
(https://hamlib.github.io, LGPL 2.1), OpenJPEG (https://www.openjpeg.org,
BSD), libwebp (https://chromium.googlesource.com/webm/libwebp, BSD),
libde265 and libheif (https://github.com/strukturag, LGPL 3) and RNNoise
(https://github.com/xiph/rnnoise, BSD), each under its licence in the
licences folder. Voice messages are coded with sotto
(https://github.com/baitisj/sotto, MIT), in sotto.dll beside ragtime.exe,
which links Codec 2 (https://github.com/drowe67/codec2, LGPL 2.1) in its
own DLL: Ragtime loads them when it starts and works without them, with no
voice coding, so you can swap in your own build. Ragtime's source and the recipe that
builds this zip (packaging/windows/make-zip.sh) are at
https://github.com/baitisj/ragtime, to build it with your own copies of
these libraries.
