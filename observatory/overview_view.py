# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inmarsat Observatory — Overview: traffic by service, every satellite's channels and their
health, ground stations, the busiest aircraft, and the channels --scan added or dropped."""

from common import SATELLITES, connect_ro, db_ready, page, sat_clause, satellites_meta, window


def build_overview(path, hours=None, sat=None):
    err = db_ready(path)
    if err:
        return err
    con = connect_ro(path)
    try:
        floor, hmax = window(con, hours)
        if floor is None:
            return {"error": "no data yet — run inmarsat-rx with --archive, then rollup.py"}
        sc, sa = sat_clause(sat)
        q = lambda sql, *a: con.execute(sql, a).fetchall()
        present = [r[0] for r in q("SELECT DISTINCT sat FROM chan_hourly UNION SELECT DISTINCT sat FROM msg_hourly")]

        kinds = {k: 0 for k in ("acars", "ack", "cpdlc", "adsc", "egc")}
        for k, c in q(f"SELECT kind,SUM(count) FROM msg_hourly WHERE hour>=?{sc} GROUP BY kind", floor, *sa):
            kinds[k] = c
        aircraft = q(f"SELECT COUNT(DISTINCT aes) FROM aircraft_hourly WHERE hour>=?{sc}", floor, *sa)[0][0]
        ges_n = q(f"SELECT COUNT(DISTINCT ges) FROM ges_hourly WHERE hour>=?{sc}", floor, *sa)[0][0]

        hourly = {}
        for h, svc, kind, c in q(f"SELECT hour,svc,kind,SUM(count) FROM msg_hourly WHERE hour>=?{sc} "
                                 f"AND kind IN ('acars','egc') GROUP BY hour,svc,kind", floor, *sa):
            hourly.setdefault(h, {"hour": h})[svc] = hourly.get(h, {}).get(svc, 0) + c
        for h in range(floor, hmax + 1, 3600):
            hourly.setdefault(h, {"hour": h})
        apps = {}
        for h, kind, c in q(f"SELECT hour,kind,SUM(count) FROM msg_hourly WHERE hour>=?{sc} "
                            f"AND kind IN ('cpdlc','adsc') GROUP BY hour,kind", floor, *sa):
            apps.setdefault(h, {"hour": h})[kind] = c
        for h in range(floor, hmax + 1, 3600):
            apps.setdefault(h, {"hour": h})

        sats = {}
        for (s, svc, freq, frames, ok, bad, msgs, minutes, sync, snr_s, snr_n, ber_s, ber_n, found,
             last) in q(f"SELECT sat,svc,freq,SUM(frames),SUM(ok),SUM(bad),SUM(msgs),SUM(minutes),"
                        f"SUM(sync_minutes),SUM(snr_sum),SUM(snr_n),SUM(ber_sum),SUM(ber_n),MAX(found),"
                        f"MAX(hour) FROM chan_hourly WHERE hour>=?{sc} GROUP BY sat,svc,freq", floor, *sa):
            sats.setdefault(s, []).append({
                "freq": freq, "svc": svc, "frames": frames, "ok": ok, "bad": bad, "msgs": msgs,
                "sync_pct": 100.0 * sync / minutes if minutes else None,
                "snr": snr_s / snr_n if snr_n else None, "ber": ber_s / ber_n if ber_n else None,
                "found": found, "last": last})
        stations = {}
        for s, freq, les, station in q("SELECT sat,freq,les,station FROM stations"):
            stations.setdefault(s, []).append({"freq": freq, "les": les, "station": station})
        sat_list = [{"id": s, **SATELLITES.get(s, {"name": s, "slot": "", "region": ""}),
                     "channels": sorted(chs, key=lambda c: (c["svc"], c["freq"])), "ncs": stations.get(s, [])}
                    for s, chs in sorted(sats.items())]

        ges = [{"ges": g, "count": c, "last": last, "sats": sats_, "aircraft": n}
               for g, c, last, sats_, n in q(
                   f"SELECT g.ges,SUM(g.count),MAX(g.hour),GROUP_CONCAT(DISTINCT g.sat),"
                   f"(SELECT COUNT(DISTINCT aes) FROM aircraft_hourly a WHERE a.ges=g.ges AND a.hour>=?{sc}) "
                   f"FROM ges_hourly g WHERE g.hour>=?{sc.replace('sat', 'g.sat')} GROUP BY g.ges ORDER BY 2 DESC",
                   floor, *sa, floor, *sa)]
        top = [{"aes": aes, "reg": reg, "count": c, "svcs": svcs, "sats": s_, "last": last}
               for aes, reg, c, svcs, s_, last in q(
                   f"SELECT aes,MAX(reg),SUM(count),GROUP_CONCAT(DISTINCT svc),GROUP_CONCAT(DISTINCT sat),"
                   f"MAX(hour) FROM aircraft_hourly WHERE hour>=?{sc} GROUP BY aes ORDER BY 3 DESC LIMIT 40",
                   floor, *sa)]
        by_svc = {svc: n for svc, n in q(f"SELECT svc,COUNT(DISTINCT aes) FROM aircraft_hourly WHERE hour>=?{sc} "
                                         f"GROUP BY svc", floor, *sa)}
        only_l = q(f"SELECT COUNT(*) FROM (SELECT aes FROM aircraft_hourly WHERE hour>=?{sc} GROUP BY aes "
                   f"HAVING SUM(svc LIKE 'aerol%')>0 AND SUM(svc='aero')=0)", floor, *sa)[0][0]
        events = [{"t": t, "op": op, "freq": f, "svc": s_, "sat": st, "why": why} for t, op, f, s_, st, why in q(
            f"SELECT t,op,freq,svc,sat,why FROM chan_events WHERE t>=? AND op IN ('add','drop'){sc} "
            f"ORDER BY t DESC LIMIT 40", floor, *sa)]
        last_data = q("SELECT MAX(t) FROM messages")[0][0]
        return {"satellites": satellites_meta(), "sats_present": present, "floor": floor, "hmax": hmax,
                "kinds": kinds, "aircraft": aircraft, "ges_count": ges_n, "aircraft_by_svc": by_svc,
                "aircraft_only_aerol": only_l,
                "hourly": [hourly[h] for h in sorted(hourly)], "apps": [apps[h] for h in sorted(apps)],
                "sats": sat_list, "ges": ges, "top": top, "events": events, "last_data": last_data}
    finally:
        con.close()


BODY = """
  <div class="cards" id="cards"></div>
  <div class="panel">
    <h2>Messages per hour <span class="hint">ACARS (not acknowledgements) by service, and STD-C EGC broadcasts</span></h2>
    <div class="legend" id="leg"></div><div id="hourly"></div>
  </div>
  <div class="panel">
    <h2>Satellites and channels <span class="hint">in window: share of minutes in frame sync, Aero signal units or STD-C packets with a good checksum, mean BER and SNR</span></h2>
    <div id="sats"></div>
  </div>
  <div class="grid">
    <div class="panel">
      <h2>Aircraft <span class="hint">most messages in window; P-channels carry ground-to-aircraft traffic</span></h2>
      <div class="scroll" id="top"></div>
    </div>
    <div>
      <div class="panel">
        <h2>Ground stations <span class="hint">GES sending to aircraft, in window</span></h2>
        <div id="ges"></div>
      </div>
      <div class="panel">
        <h2>FANS-1/A per hour <span class="hint">CPDLC and ADS-C found by libacars</span></h2>
        <div class="legend" id="appleg"></div><div id="apps"></div>
      </div>
      <div class="panel">
        <h2>Channel changes <span class="hint">carriers --scan added or dropped</span></h2>
        <div id="events"></div>
      </div>
    </div>
  </div>
"""

SCRIPT = r"""
function render(D){
  const k=D.kinds,span=(D.hmax-D.floor)/3600+1;
  $("cards").innerHTML=[card("ACARS",fmt(k.acars),fmt(k.ack)+" acks"),card("Aircraft",fmt(D.aircraft),
    (D.aircraft_only_aerol?fmt(D.aircraft_only_aerol)+" only on Aero-L":"distinct AES")),
    card("CPDLC",'<span class="hotc">'+fmt(k.cpdlc)+'</span>'),card("ADS-C",fmt(k.adsc),"contracts"),
    card("EGC",fmt(k.egc),"safety broadcasts"),card("Ground stations",D.ges_count),
    card("Satellites",D.sats.length),card("Span",span+"h",dt(D.floor).slice(0,5)+"–"+dt(D.hmax+3599).slice(0,5))].join("");
  const keys=ORDER.filter(s=>D.hourly.some(r=>r[s]));
  legend($("leg"),keys,SVCCOL);bars($("hourly"),D.hourly,keys,SVCCOL,210);
  legend($("appleg"),["cpdlc","adsc"],{cpdlc:"#f778ba",adsc:"#d29922"},{cpdlc:"CPDLC",adsc:"ADS-C"});
  bars($("apps"),D.apps,["cpdlc","adsc"],{cpdlc:"#f778ba",adsc:"#d29922"},130);
  let h="";
  for(const s of D.sats){
    const ncs=s.ncs.map(n=>esc(n.station)).join(", ");
    h+='<h3>'+esc(s.name)+'<span class="hint">'+esc([s.slot,s.region].filter(Boolean).join(" · "))+(ncs?" · NCS "+ncs:"")+
      '</span></h3><table><tr><th>MHz</th><th>Service</th><th class="r">In sync</th><th class="r">Frames</th><th class="r">Units OK</th>'+
      '<th class="r">BER</th><th class="r">SNR</th><th class="r">Messages</th><th class="r">Last heard</th></tr>';
    const cs=s.channels.slice().sort((a,b)=>ORDER.indexOf(a.svc)-ORDER.indexOf(b.svc)||a.freq-b.freq);
    for(const c of cs){const tot=c.ok+c.bad,pct=tot?100*c.ok/tot:null,sy=c.sync_pct;
      const col=v=>v>95?"var(--good)":v>80?"var(--warn)":"var(--hot)";
      h+='<tr><td class="mono">'+mhz(c.freq)+(c.found?' <span class="n" title="found by --scan">·scan</span>':'')+'</td><td>'+svc(c.svc)+
        '</td><td class="r">'+(sy==null?"–":'<span class="bar" style="width:'+Math.round(sy/2.5)+'px;background:'+col(sy)+'"></span>'+sy.toFixed(0)+'%')+
        '</td><td class="r n">'+fmt(c.frames)+'</td><td class="r">'+(pct==null?"–":'<span style="color:'+col(pct)+'">'+pct.toFixed(1)+'%</span>')+
        '</td><td class="r n">'+(c.ber==null?"–":(100*c.ber).toFixed(2)+"%")+'</td><td class="r n">'+(c.snr==null?"–":c.snr.toFixed(1)+" dB")+
        '</td><td class="r">'+fmt(c.msgs)+'</td><td class="r n">'+dt(c.last).slice(0,11)+'</td></tr>';}
    h+='</table>';
  }
  $("sats").innerHTML=h||'<div class="empty">no channel health in this window</div>';
  $("top").innerHTML=D.top.length?'<table><tr><th>Tail</th><th>AES</th><th>Heard on</th><th class="r">Messages</th><th class="r">Last</th></tr>'+
    D.top.map(a=>'<tr><td class="mono"><a style="color:inherit" href="/aviation#'+esc(a.aes)+'">'+esc(a.reg||"—")+'</a></td><td class="mono n">'+esc(a.aes)+'</td><td>'+
      a.svcs.split(",").sort((x,y)=>ORDER.indexOf(x)-ORDER.indexOf(y)).map(svc).join(" ")+
      (a.sats.includes(",")?' <span class="n">'+a.sats.split(",").map(satName).join(", ")+'</span>':'')+
      '</td><td class="r">'+fmt(a.count)+'</td><td class="r n">'+dt(a.last).slice(0,11)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">no aircraft in this window</div>';
  $("ges").innerHTML=D.ges.length?'<table><tr><th>GES</th><th>Satellites</th><th class="r">Messages</th><th class="r">Aircraft</th><th class="r">Last</th></tr>'+
    D.ges.map(g=>'<tr><td class="mono">'+esc(g.ges)+'</td><td class="n">'+g.sats.split(",").map(satName).join(", ")+'</td><td class="r">'+
      fmt(g.count)+'</td><td class="r">'+g.aircraft+'</td><td class="r n">'+dt(g.last).slice(0,11)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">none in this window</div>';
  $("events").innerHTML=D.events.length?'<table><tr><th>UTC</th><th>MHz</th><th>Service</th><th></th><th>Why</th></tr>'+
    D.events.map(e=>'<tr><td class="mono n">'+dt(e.t)+'</td><td class="mono">'+mhz(e.freq)+'</td><td>'+svc(e.svc)+'</td><td class="'+
      (e.op==="add"?"ok":"warnc")+'">'+(e.op==="add"?"added":"dropped")+'</td><td class="n">'+esc(e.why)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">no changes in this window</div>';
}
"""

PAGE = page("Inmarsat Observatory", "overview", "L-band Aero and STD-C", BODY, SCRIPT, "/api/overview")
