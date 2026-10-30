# inmarsat-rx

A standalone Inmarsat L-band decoder in C: **Aero ACARS** (Aero-H+ 10.5 kbit/s OQPSK and
Aero-L 600/1200 bit/s MSK P-channels, with FANS-1/A CPDLC and ADS-C through libacars) and
**STD-C EGC** (SafetyNET / FleetNET). It decodes every channel you list at once, from an SDR, a
network source or an IQ file, and writes one JSON record per message to stdout and/or UDP.

The DSP and decoders are ported from the SDR Workbench's browser decoders and give the same
results on the same recordings; see [Tests](#tests).

## Build

```bash
sudo apt install build-essential cmake pkg-config libairspy-dev libhackrf-dev
# libacars-2 (optional, for CPDLC/ADS-C): https://github.com/szpajder/libacars
cmake -S . -B build && cmake --build build -j
```

libacars, libairspy and libhackrf are each optional; CMake reports what it found. Without
libacars, ACARS text still decodes but CPDLC/ADS-C payloads are not expanded. RTL-SDR is reached
through `rtl_tcp` (start `rtl_tcp -a 127.0.0.1` next to the dongle).

## Run

```bash
# Aero-L and Aero-H+ on Inmarsat I-4 F3 (98°W) from an Airspy R2 with an LNA on the coax
inmarsat-rx -i airspy -b -c 1545.1697:aerol600 -c 1545.0745:aerol1200 -c 1546.005:aero -c 1546.020:aero

# STD-C from Alphasat over a SpyServer node
inmarsat-rx -i spyserver:HOST:PORT -c 1537.1056:stdc

# A recording (SigMF metadata supplies rate, centre and format)
inmarsat-rx -i capture.sigmf-meta -c 1545.1697:aerol600

# Feed a collector over UDP, nothing on stdout
inmarsat-rx -i airspy -b -c 1545.1697:aerol600 -u collector.example:5555 -q
```

Channel types:

| Type | Signal | Width | Tune within |
|---|---|---|---|
| `aero` | Aero-H+ P-channel, 10.5 kbit/s OQPSK | ~10 kHz | ±2.5 kHz |
| `aerol1200` | Aero-L P-channel, 1200 bit/s MSK | ~1.8 kHz | ±1.2 kHz |
| `aerol600` | Aero-L P-channel, 600 bit/s MSK | ~0.9 kHz | ±1.2 kHz |
| `stdc` | STD-C NCS TDM, 1200 Bd BPSK | ~2.5 kHz | ±2.5 kHz |

Aero-L carriers often sit 5 kHz apart, so their carrier search is narrower. For live sources the
program picks the centre (midway between the channels, kept off every carrier) and the lowest
sample rate that covers them; `-F` and `-s` override. `inmarsat-rx -h` lists every option.

Status per channel goes to stderr every 30 s of samples (`--stats`): lock, carrier offset, SNR,
frames, channel BER, signal units or packets, messages.

## Dashboard

Like iridium-observatory and vdl2-observatory, inmarsat-rx comes with two pages:

| Port | What | Shows |
|---|---|---|
| 8892 | **Live** (`inmarsat-rx --web 8892`) | what the receiver hears now: every channel by satellite and service (sync, SNR, BER, units OK), ACARS with CPDLC/ADS-C decoded, aircraft, ground stations, STD-C EGC |
| 8893 | **Observatory** (`observatory/server.py`) | history from the archive: Overview (traffic by service, channel health per satellite, ground stations, busiest aircraft, channels `--scan` added), Aviation (per-aircraft history, CPDLC and ADS-C logs), Maritime (EGC by priority, STD-C ship traffic as metadata) |
| 8894 | **STD-C messages**, opt-in (`--stdc-content DIR`) | messages to individual ships, on 127.0.0.1 only: see [STD-C ship traffic](#std-c-ship-traffic) |

Every record carries a satellite and a service (Aero-H+, Aero-L 600/1200, STD-C), and both pages
filter by either. The Observatory reads a database that `observatory/rollup.py` folds from the
archive (`--archive DIR`), once a minute under systemd.

```bash
./install.sh          # packages, build, tests, then the three services; asks a few questions
```

It asks for the source (`airspy`, `hackrf`, `rtl_tcp:HOST:PORT`, `spyserver:HOST:PORT`), the
satellite plans, gain, bias-tee, whether to scan for more carriers, UDP feeds and a station name,
and writes them to `/etc/inmarsat-observatory.conf`. `./install.sh --update`, `--reconfigure`,
`--uninstall` and `--dry-run` work as vdl2-observatory's do. One SDR serves one program: on a box
where iridium-sniffer already has the Airspy, give inmarsat-rx its own SDR.

## Channels

`--plan i4f3` (or `i3f5`, `alphasat`, several comma-separated, or `all`) takes a satellite's known
carriers; those outside the tuned band are skipped. `--plans` lists them. `--scan` averages the
spectrum, tries every carrier above the noise as the types its width allows (0.9 kHz: Aero-L 600;
1.5–2.5 kHz: Aero-L 1200 or STD-C; ~10 kHz: Aero-H+), keeps those that reach frame sync, and drops
them again after 15 minutes without frames. On the 120 s I-4 F3 test recording it found 1545.195
and 1545.205, two Aero-L 600 carriers missing from the plan.

### Land earth station names

STD-C pages show an LES by name where this receiver has seen the name: LES 2 on I-4 F3 is Stratos
CSAT and LES 12 NL Burum on both satellites (each from the header of a message the station sent),
and LES 44 is the NCS. Others show as "LES n". To add or correct names, put a `les_names.json`
beside the Observatory database, e.g. `{"i3f5:4": "Name"}` (satellite id, colon, LES number); the
Observatory reads it again when it changes.

## STD-C ship traffic

Besides EGC broadcasts, the STD-C channels carry signalling for individual ships: messages
announced to a ship, logical channel assignments, acknowledgements, polls. These carry the ship's
mobile number, so inmarsat-rx never records it as such:

- **Metadata** (the archive, the live page and the Observatory's Maritime page) shows each ship as a
  keyed hash: HMAC-SHA256 of its mobile number with a secret key, `~/.config/inmarsat-rx/mobile.key`
  (made on first use, mode 600; `--mobile-key FILE` to choose another). The same ship keeps the same
  hash on one receiver, but the number cannot be recovered or matched without the key, so keep the
  key out of anything you share. Without a key, packets that carry a mobile number are not
  recorded at all.
- **Message content** is off unless asked for. `--stdc-content DIR` rebuilds the messages to ships
  from the land earth stations' channels for a page served on **127.0.0.1 only**
  (`--content-port`, default 8894; reach it through an SSH tunnel). Deliveries are kept 24 hours in
  `DIR` (mode 700, files 600) and are never archived, sent over UDP, written to stdout or shown on
  the shared pages. Ships appear as the same keyed hash. Text is shown as sent; binary content is
  described (size, how random the bytes are, keyed hashes to group identical payloads and those that
  open alike) but no byte of it is shown or kept. Each delivery shows whether the ship sent
  something soon after (a channel assignment and an acknowledged message).

## Output

One JSON object per line, the SDR Workbench message-log record (v1):

```json
{"v":1,"key":"aero:C051E6:A6:J:…","decoder":"aero","type":"acars","link":false,
 "firstSeen":1791…,"lastSeen":1791…,"count":1,"source":{"kind":"sdr","name":"Airspy "},
 "freqHz":1545169700,"aircraft":{"reg":"C-GFAJ","flight":null,"address":"C051E6"},
 "ground":{"id":"D0","name":null},"from":"GES D0","to":"C-GFAJ","label":"A6","block":"J",
 "app":{"kind":"ADS-C","summary":"…","error":false},"position":{"lat":…,"lon":…,"alt":…},
 "text":"/YQXE2YA.ADS.C-GFAJ07010BCC…","decode":{"tree":{…libacars JSON…},"text":"…"},
 "meta":{"mode":"2","tak":"\u0015","rate":600,"more":false}}
```

- `link: true` marks ACARS link acknowledgements (label `_d`) and blocks with no text; leave them
  out with `--hide-acks`.
- STD-C records have `type: "egc"` with `service`, `priority`, `egcId`, `parts`, `complete`. An EGC
  message is written once, when its last part arrives, or after 10 minutes with
  `complete: false` and `[…]` where parts are missing.
- `--data` adds non-ACARS Aero user data (hex), `--bulletins` the STD-C bulletin board of every
  frame.
- `meta.sat` and `meta.channel` name the satellite and service of every record.
- `--blocks` writes one record per ACARS block instead of joining multi-block messages
  (`meta.more` is true on blocks that end with ETB), as acarsdec-style feeds do.
- Record times count from the first sample: SigMF `core:datetime`, else `--start TIME` for a raw
  recording, else the moment the program started.

## Tests

On the SDR Workbench's 120 s I-4 F3 recording (Airspy R2, 1546.04 MHz, 2.5 MS/s), with every
carrier listed: eleven Aero-L carriers (ten 600, one 1200 bit/s) at 0.0 % channel BER, 41 ACARS
blocks, identical to the workbench decoder carrier by carrier; four Aero-H+ channels, ~235 frames
each at 0.4–1.3 % BER. Thirteen channels decode about 10× faster than real time on 6 cores. On the
live I-4 F3 STD-C recording: 12 frames, 28 packets, as the workbench logged. Live over a SpyServer
node: Alphasat STD-C, 0.0 % BER.

The carrier and timing loops act sample by sample, so results do not depend on how the input is
blocked (`--block`).

## Credits

See NOTICE. GNU General Public License v3.0 or later.
