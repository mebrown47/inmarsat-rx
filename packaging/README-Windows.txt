inmarsat-rx for Windows (64-bit)
================================

inmarsat-rx decodes Inmarsat L-band signals: Aero ACARS (Aero-L 600/1200 bit/s
and Aero-H+ 10.5 kbit/s) and STD-C EGC safety broadcasts. It is one program,
inmarsat-rx.exe, with nothing to install. Run it from a Command Prompt or
PowerShell window in this folder.


1. Check that it works
----------------------

    inmarsat-rx.exe -i sample\aerol600-i4f3-12k5.sigmf-meta -c 1545.060:aerol600:i4f3 -c 1545.065:aerol600:i4f3

This decodes 34 seconds of a recording from Inmarsat I-4 F3. It should print
five ACARS messages (one JSON object per line), and the last two status lines
should show "SYNC frames 16 ber 0.0%" for both channels.

    inmarsat-rx.exe -h          lists every option
    inmarsat-rx.exe --plans     lists the known carriers per satellite


2. Which satellite you can see
------------------------------

You need an L-band antenna with an LNA (an Inmarsat patch antenna, say),
pointed at a satellite with a clear view in that direction.

  Where you are       Satellite       Plan       Look angle (approximate)
  ---------------     -------------   --------   -----------------------------
  North America       I-4 F3, 98 W    i4f3       south to south-west
  UK                  Alphasat, 25 E  alphasat   azimuth 148 (SSE), elevation 21-27
  UK, low horizon     I-3 F5, 54 W    i3f5       azimuth 238 (WSW), elevation 12-13
  Europe, Africa      Alphasat, 25 E  alphasat   south-east to south

(Elevation is lowest in Scotland, highest in southern England. I-4 F3 is below
the horizon from the UK.)

The program knows I-4 F3's carriers well. For Alphasat it knows only the STD-C
signalling channel, 1537.1056 MHz, so outside the Americas use --scan: it
looks for carriers in the band you tune, tries each one, and keeps those that
decode. Please send back what it finds (section 4).


3. Connecting the SDR
---------------------

This build reaches the radio over the network, on the same PC or another:

RTL-SDR: install the WinUSB driver once with Zadig (https://zadig.akeo.ie:
Options > List All Devices, "Bulk-In, Interface (Interface 0)", WinUSB,
Replace Driver), then start rtl_tcp from the Windows RTL-SDR tools (not
included here) in a window of its own:

    rtl_tcp -a 127.0.0.1 -s 1024000

and use  -i rtl_tcp:127.0.0.1:1234 -s 1024000  below.

Airspy: run Airspy's SpyServer, and use  -i spyserver:127.0.0.1:5555  below
(leave out -s; the program picks a rate the server offers).

Airspy and HackRF over USB directly are not in this build yet.

Add -b to power the LNA through the SDR's bias-tee, if it has one and your
LNA needs it. Add --web 8892 and open http://localhost:8892/ for the live page.

UK and Europe, Alphasat:

  STD-C safety broadcasts (start here: one known carrier, strong and steady):

    inmarsat-rx.exe -i rtl_tcp:127.0.0.1:1234 -s 1024000 -F 1537.3 --plan alphasat --scan --web 8892

  Aero (ACARS to aircraft), searching 1545.0-1546.0 MHz:

    inmarsat-rx.exe -i rtl_tcp:127.0.0.1:1234 -s 1024000 -F 1545.5 --sat alphasat --scan --web 8892

  For I-3 F5 instead: --plan i3f5 -F 1541.7 (its STD-C carrier is 1541.45 MHz;
  tune a little off a carrier, as RTL-SDRs have a spike at their centre).

North America, I-4 F3, Aero-L:

    inmarsat-rx.exe -i rtl_tcp:127.0.0.1:1234 -s 1024000 -F 1545.2 --plan i4f3 --web 8892

Each status line (every 30 s) shows, per channel, whether it has frame sync
("SYNC"), the signal-to-noise ratio and the error rate. --scan prints
"scan: ... is aerol600 ... kept" (or stdc, aero) for each carrier it finds.

Some PCs lose samples from an RTL-SDR above about 1 MS/s; 1.024 MS/s covers
1 MHz. If the status lines show "Dropped ... samples", try another USB port or
the High performance power plan.


4. Sending results back
-----------------------

This is a pre-release. What helps most is the status output from a run of
20-30 minutes. In a Command Prompt (not PowerShell, see below):

    inmarsat-rx.exe ...your options... --stats 60 -q --archive results 2> status.txt

then send status.txt and the results folder. They contain the carriers found,
signal levels and the decoded messages (ACARS and safety broadcasts); STD-C
ships appear only as hashes made with your own secret key (see below).


5. Things to know on Windows
----------------------------

- The program is not code-signed yet. Windows may say "Windows protected your
  PC" on first run: choose More info, then Run anyway. Some antivirus programs
  are wary of new unsigned programs too.
- Windows may ask whether inmarsat-rx may use the network the first time you
  use --web. Allowing it on private networks is enough.
- PowerShell's ">" writes files in UTF-16, which programs reading JSON lines
  do not expect. Use --archive FOLDER (one UTF-8 file per day), a UDP feed
  (-u HOST:PORT), or "| Out-File -Encoding utf8" instead.
- The secret key that hashes ships' mobile numbers in STD-C metadata is kept
  in %APPDATA%\inmarsat-rx\mobile.key. Keep it out of anything you share. The
  optional STD-C message page (--stdc-content FOLDER) keeps ships' messages
  for 24 hours: put that folder inside your own user profile.
- Not in this build yet: Airspy and HackRF over USB (use rtl_tcp or a
  SpyServer), and CPDLC/ADS-C decoding (ACARS text is decoded; the CPDLC
  and ADS-C contents are not expanded).


Licence and source
------------------

GNU General Public License v3.0 or later (LICENSE; credits in NOTICE).
Source code: https://github.com/mebrown47/inmarsat-rx
This build: see VERSION.txt.
