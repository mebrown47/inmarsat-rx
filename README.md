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

See NOTICE. Apache License 2.0.
