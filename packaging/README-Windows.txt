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


2. Live reception with an RTL-SDR
---------------------------------

You need an L-band antenna with an LNA (an Inmarsat patch antenna, say),
pointed at the satellite. From North America that is I-4 F3 at 98 W.

a. Install the WinUSB driver for the dongle, once: run Zadig
   (https://zadig.akeo.ie), choose Options > List All Devices, pick
   "Bulk-In, Interface (Interface 0)", select WinUSB, and click Replace Driver.

b. Start rtl_tcp (part of the RTL-SDR tools for Windows) in one window:

       rtl_tcp -a 127.0.0.1 -s 1024000

c. Run inmarsat-rx against it in another window (-b powers the LNA through
   the dongle's bias-tee, on dongles that have one):

       inmarsat-rx.exe -i rtl_tcp:127.0.0.1:1234 -s 1024000 -F 1545.2 --plan i4f3 -b --web 8892

   and open http://localhost:8892/ in a browser for the live page.

Some laptops lose samples from an RTL-SDR above about 1 MS/s; 1.024 MS/s covers
1 MHz, enough for a group of Aero-L carriers or STD-C. If the status lines show
"Dropped ... samples", try a lower rate, another USB port, or the High
performance power plan.

SpyServer nodes work the same way: -i spyserver:HOST:PORT.


3. Things to know on Windows
----------------------------

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
