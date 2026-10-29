#!/usr/bin/env python3
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Inmarsat Observatory — archive rollup

Folds the append-only JSONL archive written by inmarsat-rx's --archive flag into hourly
aggregates and per-message rows in a SQLite database, so the dashboard can query weeks of
history cheaply. Adapted from vdl2-observatory's rollup.py, which follows iridium-sniffer's.

Design (as vdl2-observatory):
  * Incremental. Per-file byte offsets are tracked in `ingest_state`, so each run only reads
    bytes appended since last time. Re-running is safe and cheap.
  * Only complete lines (terminated by '\\n') are consumed; a partial trailing line waits.
  * Aggregates are UPSERT-added, so a line counted once lands in exactly one hour bucket.
  * Deleting the DB triggers a clean full re-ingest.

Every row carries the satellite (inmarsat-rx's satellite id, e.g. i4f3) and the service (the
channel type: aero, aerol600, aerol1200, stdc), so every page can be cut by either.

Time units: the archive carries `t` in **milliseconds**; every DB timestamp is **epoch
seconds**. Convert once, at the boundary, via sec_of()/hour_of().

Usage:
    rollup.py [--archive DIR] [--db PATH] [--verbose]
"""

import argparse
import glob
import json
import os
import sqlite3
import sys
import time

SCHEMA_VERSION = 1


def sec_of(t_ms):
    """Epoch seconds for the archive's ms stamp. The only ms->s boundary."""
    return int(t_ms) // 1000


def hour_of(t_ms):
    return sec_of(t_ms) // 3600 * 3600


def init_db(con):
    con.executescript(
        """
        PRAGMA journal_mode=WAL;
        PRAGMA synchronous=NORMAL;

        CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE IF NOT EXISTS ingest_state (
            file TEXT PRIMARY KEY, offset INTEGER NOT NULL, size INTEGER NOT NULL,
            updated INTEGER NOT NULL
        );

        -- One row per ACARS message (a multi-block message once, whole). P-channels are
        -- ground to aircraft, so every row is an uplink.
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            t INTEGER, sat TEXT, svc TEXT, freq INTEGER,
            aes TEXT, ges TEXT, reg TEXT, lbl TEXT, blk TEXT, txt TEXT,
            ack INTEGER, app TEXT, app_sum TEXT
        );
        CREATE INDEX IF NOT EXISTS ix_messages_t ON messages(t);
        CREATE INDEX IF NOT EXISTS ix_messages_aes ON messages(aes);
        CREATE INDEX IF NOT EXISTS ix_messages_reg ON messages(reg);

        CREATE TABLE IF NOT EXISTS cpdlc_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            t INTEGER, sat TEXT, svc TEXT, aes TEXT, reg TEXT, ges TEXT,
            txt TEXT, summary TEXT, dec TEXT
        );
        CREATE INDEX IF NOT EXISTS ix_cpdlc_t ON cpdlc_log(t);
        CREATE TABLE IF NOT EXISTS adsc_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            t INTEGER, sat TEXT, svc TEXT, aes TEXT, reg TEXT, ges TEXT,
            txt TEXT, kinds TEXT, summary TEXT, dec TEXT
        );
        CREATE INDEX IF NOT EXISTS ix_adsc_t ON adsc_log(t);

        -- STD-C EGC broadcasts, one row each.
        CREATE TABLE IF NOT EXISTS egc (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            t INTEGER, sat TEXT, freq INTEGER, les INTEGER, station TEXT,
            svc INTEGER, service TEXT, prio TEXT, egc_id INTEGER, parts INTEGER,
            complete INTEGER, rep INTEGER, txt TEXT
        );
        CREATE INDEX IF NOT EXISTS ix_egc_t ON egc(t);

        -- Channel health from the per-minute "imix" event. minutes: minutes reported;
        -- sync_minutes: of those, in frame sync at the minute's end.
        CREATE TABLE IF NOT EXISTS chan_hourly (
            hour INTEGER, sat TEXT, svc TEXT, freq INTEGER,
            frames INTEGER, ok INTEGER, bad INTEGER, msgs INTEGER,
            minutes INTEGER, sync_minutes INTEGER,
            snr_sum REAL, snr_n INTEGER, ber_sum REAL, ber_n INTEGER, found INTEGER,
            PRIMARY KEY (hour, sat, svc, freq)
        );
        -- Messages per hour by kind: acars (not acks), ack, cpdlc, adsc, egc.
        CREATE TABLE IF NOT EXISTS msg_hourly (
            hour INTEGER, sat TEXT, svc TEXT, kind TEXT, count INTEGER,
            PRIMARY KEY (hour, sat, svc, kind)
        );
        CREATE TABLE IF NOT EXISTS aircraft_hourly (
            hour INTEGER, aes TEXT, sat TEXT, svc TEXT, count INTEGER, reg TEXT, ges TEXT,
            PRIMARY KEY (hour, aes, sat, svc)
        );
        CREATE TABLE IF NOT EXISTS ges_hourly (
            hour INTEGER, ges TEXT, sat TEXT, count INTEGER,
            PRIMARY KEY (hour, ges, sat)
        );
        -- Channels added or dropped by --scan, and receiver starts.
        CREATE TABLE IF NOT EXISTS chan_events (
            t INTEGER, op TEXT, freq INTEGER, svc TEXT, sat TEXT, why TEXT
        );
        -- STD-C packet tally per channel and descriptor byte (count, bytes), from the per-minute
        -- "spkt" event. Metadata only: the archive holds no packet contents.
        CREATE TABLE IF NOT EXISTS stdc_pkt_hourly (
            hour INTEGER, sat TEXT, freq INTEGER, desc TEXT, count INTEGER, bytes INTEGER,
            PRIMARY KEY (hour, sat, freq, desc)
        );
        -- The NCS each STD-C channel belongs to, from its bulletin board.
        CREATE TABLE IF NOT EXISTS stations (
            sat TEXT, freq INTEGER, les INTEGER, station TEXT, t INTEGER,
            PRIMARY KEY (sat, freq)
        );
        """
    )
    con.execute("INSERT INTO meta(key,value) VALUES('schema',?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (str(SCHEMA_VERSION),))
    con.commit()


def find_node(obj, name):
    """The first sub-object whose key contains `name`, else None (libacars names its nodes
    after the message type)."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            if name in k:
                return v
            r = find_node(v, name)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = find_node(v, name)
            if r is not None:
                return r
    return None


# Readable names for the ADS-C tags libacars emits (as vdl2-observatory); unmapped tags show raw.
_ADSC_LABELS = {
    "periodic_contract_req": "periodic contract", "event_contract_req": "event contract",
    "demand_contract_req": "demand contract", "cancel_contract_req": "cancel contract",
    "cancel_all_contracts_req": "cancel all contracts", "cancel_emergency_mode_req": "cancel emergency",
    "report_wpt_changes": "wpt changes", "report_when_alt_out_of_range": "alt band",
    "report_when_lateral_dev_exceeds": "lateral dev", "report_when_vspd_out_of_range": "vspd band",
    "report_when_airspeed_change_exceeds": "airspeed change", "report_interval": "interval",
    "earth_ref_data": "earth ref", "air_ref_data": "air ref", "meteo_data": "meteo",
    "basic_report": "position", "flight_id": "flight id", "predicted_route": "predicted route",
    "ack": "ack", "nack": "nack", "noncompliance_notify": "noncompliance",
}


def adsc_tags(node):
    out = []
    tags = node.get("tags") if isinstance(node, dict) else None
    for tag in tags or []:
        if isinstance(tag, dict):
            out.extend(tag.items())
    return out


def adsc_summary(node, limit=12):
    parts = []

    def emit(name, body):
        if len(parts) >= limit:
            return
        bits = []
        if isinstance(body, dict):
            if isinstance(body.get("contract_num"), int):
                bits.append(f"#{body['contract_num']}")
            f, c = body.get("floor_alt"), body.get("ceiling_alt")
            if f is not None and c is not None:
                bits.append(f"{f}–{c} ft")
            if isinstance(body.get("interval_secs"), int):
                bits.append(f"{body['interval_secs']}s")
        parts.append((_ADSC_LABELS.get(name, name) + " " + " ".join(bits)).strip())
        if isinstance(body, dict):
            for g in body.get("groups") or []:
                if isinstance(g, dict):
                    for gn, gb in g.items():
                        emit(gn, gb)

    for name, body in adsc_tags(node):
        emit(name, body)
    return " · ".join(parts[:limit])


class Agg:
    """In-memory accumulator; flushed to the DB as UPSERT-adds."""

    def __init__(self):
        self.chan = {}      # (hour,sat,svc,freq) -> [frames,ok,bad,msgs,minutes,sync,snr_sum,snr_n,ber_sum,ber_n,found]
        self.msg = {}       # (hour,sat,svc,kind) -> count
        self.ac = {}        # (hour,aes,sat,svc) -> [count, reg, ges]
        self.ges = {}       # (hour,ges,sat) -> count
        self.messages, self.cpdlcs, self.adscs, self.egcs, self.events = [], [], [], [], []
        self.stations = {}  # (sat,freq) -> (les, station, t)
        self.spkt = {}      # (hour,sat,freq,desc) -> [count, bytes]
        self.lines = self.bad = 0

    def add(self, d, key, n=1):
        d[key] = d.get(key, 0) + n

    def event(self, ev):
        e = ev.get("e")
        if e == "imix":
            h = hour_of(ev["t"])
            for freq, c in ev.get("ch", {}).items():
                k = (h, c.get("sat", "?"), c.get("svc", "?"), int(freq))
                a = self.chan.setdefault(k, [0] * 11)
                a[0] += c.get("fr", 0); a[1] += c.get("ok", 0); a[2] += c.get("bad", 0); a[3] += c.get("msg", 0)
                a[4] += 1; a[5] += 1 if c.get("sync") else 0
                if c.get("snr"):
                    a[6] += c["snr"]; a[7] += 1
                if c.get("ber") is not None and c.get("fr"):
                    a[8] += c["ber"]; a[9] += 1
                a[10] = max(a[10], c.get("found", 0))
                if c.get("les") is not None:
                    self.stations[(c.get("sat", "?"), int(freq))] = (c["les"], c.get("station", ""), sec_of(ev["t"]))
        elif e == "acars":
            h, ts = hour_of(ev["t"]), sec_of(ev["t"])
            sat, svc = ev.get("sat", "?"), ev.get("svc", "?")
            ack = ev.get("lbl") == "_d"
            app = ev.get("app")
            self.add(self.msg, (h, sat, svc, "ack" if ack else "acars"))
            if app == "CPDLC":
                self.add(self.msg, (h, sat, svc, "cpdlc"))
            elif app == "ADS-C":
                self.add(self.msg, (h, sat, svc, "adsc"))
            aes, ges, reg = ev.get("aes", ""), ev.get("ges", ""), (ev.get("reg") or "").lstrip(".")
            a = self.ac.setdefault((h, aes, sat, svc), [0, "", ""])
            a[0] += 1
            if reg:
                a[1] = reg
            a[2] = ges
            self.add(self.ges, (h, ges, sat))
            txt = ev.get("txt") or ""
            self.messages.append((ts, sat, svc, ev.get("freq", 0), aes, ges, reg, ev.get("lbl", ""),
                                  ev.get("blk", ""), txt, int(ack), app, ev.get("app_sum")))
            dec = ev.get("dec")
            if app == "CPDLC":
                node = find_node(dec, "cpdlc") if dec else None
                self.cpdlcs.append((ts, sat, svc, aes, reg, ges, txt, ev.get("app_sum") or "",
                                    json.dumps(node, separators=(",", ":")) if node is not None else ""))
            elif app == "ADS-C":
                node = find_node(dec, "adsc") if dec else None
                kinds = ",".join(n for n, _ in adsc_tags(node)) if node is not None else ""
                self.adscs.append((ts, sat, svc, aes, reg, ges, txt, kinds,
                                   adsc_summary(node) if node is not None else (ev.get("app_sum") or ""),
                                   json.dumps(node, separators=(",", ":")) if node is not None else ""))
        elif e == "egc":
            h, ts = hour_of(ev["t"]), sec_of(ev["t"])
            self.add(self.msg, (h, ev.get("sat", "?"), "stdc", "egc"))
            self.egcs.append((ts, ev.get("sat", "?"), ev.get("freq", 0), ev.get("les"), ev.get("station"),
                              ev.get("svc"), ev.get("service"), ev.get("prio"), ev.get("id"), ev.get("parts"),
                              int(bool(ev.get("complete"))), ev.get("rep"), ev.get("txt") or ""))
        elif e == "spkt":
            h = hour_of(ev["t"])
            for freq, c in ev.get("ch", {}).items():
                sat = c.get("sat", "?")
                for desc, (n, b) in c.get("d", {}).items():
                    a = self.spkt.setdefault((h, sat, int(freq), desc), [0, 0])
                    a[0] += n
                    a[1] += b
                if c.get("les") is not None:
                    self.stations[(sat, int(freq))] = (c["les"], c.get("station", ""), sec_of(ev["t"]))
        elif e == "chan":
            self.events.append((sec_of(ev["t"]), ev.get("op"), ev.get("f"), ev.get("svc"), ev.get("sat"), ev.get("why")))
        elif e == "start":
            self.events.append((sec_of(ev["t"]), "start", None, None, None, ev.get("src")))

    def flush(self, con):
        def upsert(table, cols, d):
            con.executemany(
                f"INSERT INTO {table}({','.join(cols)},count) VALUES({','.join('?' * len(cols))},?) "
                f"ON CONFLICT({','.join(cols)}) DO UPDATE SET count=count+excluded.count",
                [(*k, v) for k, v in d.items()])

        upsert("msg_hourly", ("hour", "sat", "svc", "kind"), self.msg)
        upsert("ges_hourly", ("hour", "ges", "sat"), self.ges)
        con.executemany(
            "INSERT INTO chan_hourly VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(hour,sat,svc,freq) DO UPDATE SET frames=frames+excluded.frames, ok=ok+excluded.ok, "
            "bad=bad+excluded.bad, msgs=msgs+excluded.msgs, minutes=minutes+excluded.minutes, "
            "sync_minutes=sync_minutes+excluded.sync_minutes, snr_sum=snr_sum+excluded.snr_sum, "
            "snr_n=snr_n+excluded.snr_n, ber_sum=ber_sum+excluded.ber_sum, ber_n=ber_n+excluded.ber_n, "
            "found=MAX(found,excluded.found)",
            [(*k, *v) for k, v in self.chan.items()])
        con.executemany(
            "INSERT INTO aircraft_hourly VALUES(?,?,?,?,?,?,?) ON CONFLICT(hour,aes,sat,svc) DO UPDATE SET "
            "count=count+excluded.count, reg=CASE WHEN excluded.reg<>'' THEN excluded.reg ELSE reg END, "
            "ges=excluded.ges", [(*k, *v) for k, v in self.ac.items()])
        con.executemany("INSERT INTO messages(t,sat,svc,freq,aes,ges,reg,lbl,blk,txt,ack,app,app_sum) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", self.messages)
        con.executemany("INSERT INTO cpdlc_log(t,sat,svc,aes,reg,ges,txt,summary,dec) VALUES(?,?,?,?,?,?,?,?,?)",
                        self.cpdlcs)
        con.executemany("INSERT INTO adsc_log(t,sat,svc,aes,reg,ges,txt,kinds,summary,dec) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?)", self.adscs)
        con.executemany("INSERT INTO egc(t,sat,freq,les,station,svc,service,prio,egc_id,parts,complete,rep,txt) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", self.egcs)
        con.executemany("INSERT INTO chan_events VALUES(?,?,?,?,?,?)", self.events)
        con.executemany("INSERT INTO stdc_pkt_hourly VALUES(?,?,?,?,?,?) ON CONFLICT(hour,sat,freq,desc) DO UPDATE "
                        "SET count=count+excluded.count, bytes=bytes+excluded.bytes",
                        [(*k, *v) for k, v in self.spkt.items()])
        con.executemany("INSERT INTO stations VALUES(?,?,?,?,?) ON CONFLICT(sat,freq) DO UPDATE SET "
                        "les=excluded.les, station=excluded.station, t=excluded.t WHERE excluded.t>=stations.t",
                        [(*k, *v) for k, v in self.stations.items()])


def ingest_file(con, path, agg, verbose):
    size = os.path.getsize(path)
    row = con.execute("SELECT offset,size FROM ingest_state WHERE file=?", (path,)).fetchone()
    offset = row[0] if row else 0
    if row and size < row[1]:
        offset = 0  # File shrank or was replaced: read it again from the top.
    if offset >= size:
        return 0
    with open(path, "rb") as f:
        f.seek(offset)
        buf = f.read()
    *complete, tail = buf.split(b"\n")
    consumed = len(buf) - len(tail)
    n = 0
    for raw in complete:
        if not raw.strip():
            continue
        try:
            agg.event(json.loads(raw))
            n += 1
        except (ValueError, KeyError, TypeError):
            agg.bad += 1
    agg.lines += n
    con.execute("INSERT INTO ingest_state(file,offset,size,updated) VALUES(?,?,?,?) "
                "ON CONFLICT(file) DO UPDATE SET offset=excluded.offset, size=excluded.size, "
                "updated=excluded.updated", (path, offset + consumed, size, int(time.time())))
    if verbose:
        print(f"  {os.path.basename(path)}: +{n} events ({offset}->{offset + consumed} of {size} bytes)")
    return n


def main():
    ap = argparse.ArgumentParser(description="Inmarsat Observatory rollup")
    ap.add_argument("--archive", default="archive", help="directory of inmarsat-YYYYMMDD.jsonl files")
    ap.add_argument("--db", default="observatory/observatory.db", help="SQLite database to write")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.archive, "inmarsat-*.jsonl")))
    if not files:
        print(f"rollup: no archive files in {args.archive}", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(args.db)), exist_ok=True)
    con = sqlite3.connect(args.db)
    init_db(con)
    agg = Agg()
    total = sum(ingest_file(con, p, agg, args.verbose) for p in files)
    agg.flush(con)
    con.execute("INSERT INTO meta(key,value) VALUES('last_run',?) ON CONFLICT(key) DO UPDATE SET "
                "value=excluded.value", (str(int(time.time())),))
    con.commit()
    con.close()
    print(f"rollup: {total} new events from {len(files)} file(s)" + (f", {agg.bad} malformed" if agg.bad else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
