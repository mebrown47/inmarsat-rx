#!/usr/bin/env bash
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run_tests.sh BUILD_DIR — inmarsat-rx and the Observatory on the committed test recording:
# 34 s of two I-4 F3 Aero-L 600 bit/s P-channels (tests/data). install.sh runs this before it
# installs anything. Prints one line per check and a RESULT line.
set -uo pipefail
BUILD="${1:-build}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$BUILD/inmarsat-rx"
DATA="$HERE/data/aerol600-i4f3-12k5.sigmf-meta"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0
check() { if eval "$2"; then echo "ok   $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
CH="-c 1545.060:aerol600:i4f3 -c 1545.065:aerol600:i4f3"

[ -x "$BIN" ] || { echo "no binary at $BIN"; echo "RESULT: 0 passed, 1 failed"; exit 1; }
check "plans list I-4 F3" "'$BIN' --plans | grep -q '^  i4f3 '"

# Decode: every frame, no channel errors, and the messages a check of the recording found.
"$BIN" -i "$DATA" $CH --stats 0 > "$TMP/out.jsonl" 2> "$TMP/err.txt"
check "decodes without error" "[ \$? -eq 0 ] && [ -s '$TMP/out.jsonl' ]"
check "both channels: 16 of 17 frames at 0.0% BER" "[ \$(grep -c 'SYNC frames 16 ber 0.0% SU 96/0' '$TMP/err.txt') -eq 2 ]"
check "five ACARS messages" "[ \$(wc -l < '$TMP/out.jsonl') -eq 5 ]"
check "PT-TME route clearance, word for word" \
  "grep -qF '\"text\":\"- #MDFPN/FNTAM3668/RP:DA:SBRJ:AA:SBBR:F:MUDKA,S21343W043345.UM409..ISIPI,S17078W' '$TMP/out.jsonl'"
check "records carry satellite and service" "grep -q '\"sat\":\"i4f3\",\"channel\":\"aerol600\"' '$TMP/out.jsonl'"
if "$BIN" -i "$DATA" $CH --stats 0 2>&1 >/dev/null | grep -q 'without libacars'; then
  echo "skip CPDLC decode (built without libacars)"
else
  check "CPDLC for C-GFAJ decoded by libacars" "grep '\"reg\":\"C-GFAJ\"' '$TMP/out.jsonl' | grep -q '\"kind\":\"CPDLC\"'"
fi

# The loops act sample by sample, so how the input is blocked changes nothing.
# (Record times follow the read a message completes in, so compare the messages themselves.)
"$BIN" -i "$DATA" $CH --stats 0 --block 777 > "$TMP/small.jsonl" 2>/dev/null
"$BIN" -i "$DATA" $CH --stats 0 --block 65536 > "$TMP/big.jsonl" 2>/dev/null
keys() { sed 's/"firstSeen":[0-9]*,"lastSeen":[0-9]*,//' "$1"; }
check "same messages with 777- and 65536-sample reads" "cmp -s <(keys '$TMP/small.jsonl') <(keys '$TMP/big.jsonl')"

# Discovery alone finds both carriers.
"$BIN" -i "$DATA" --scan --scan-period 5 --sat i4f3 --stats 0 -q 2> "$TMP/scan.txt"
check "--scan finds and keeps both carriers" "[ \$(grep -c 'is aerol600' '$TMP/scan.txt') -eq 2 ]"

# Archive -> rollup -> dashboard data.
"$BIN" -i "$DATA" $CH --stats 0 -q --archive "$TMP/archive" 2>/dev/null
check "archive written" "ls '$TMP/archive'/inmarsat-2026100*.jsonl >/dev/null 2>&1"
python3 "$HERE/../observatory/rollup.py" --archive "$TMP/archive" --db "$TMP/obs.db" > "$TMP/rollup.txt" 2>&1
check "rollup runs" "grep -q 'new events' '$TMP/rollup.txt'"
python3 - "$HERE/../observatory" "$TMP/obs.db" > "$TMP/views.txt" 2>&1 <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import overview_view, aviation_view, maritime_view
o = overview_view.build_overview(sys.argv[2], "0", None)
a = aviation_view.build_aviation(sys.argv[2], "0", None, "C051E6")
m = maritime_view.build_maritime(sys.argv[2], "0", None)
assert "error" not in o and "error" not in a and "error" not in m, (o.get("error"), a.get("error"), m.get("error"))
assert o["kinds"]["acars"] == 3 and o["kinds"]["ack"] == 2, o["kinds"]
assert len(o["sats"]) == 1 and len(o["sats"][0]["channels"]) == 2, o["sats"]
assert any(h["reg"] == "C-GFAJ" for h in a["history"]), a["history"]
print("views ok")
EOF
check "Overview, Aviation and Maritime data" "grep -q 'views ok' '$TMP/views.txt'"
[ "$fail" -eq 0 ] || cat "$TMP/views.txt"

echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
