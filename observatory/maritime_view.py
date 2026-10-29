# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inmarsat Observatory — Maritime: STD-C EGC broadcasts (SafetyNET navigational and weather
warnings, distress relays, FleetNET) and the NCS each satellite's STD-C channel belongs to."""

from common import connect_ro, db_ready, page, sat_clause, satellites_meta, window


def build_maritime(path, hours=None, sat=None, prio=None):
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
        present = [r[0] for r in q("SELECT DISTINCT sat FROM msg_hourly")]
        by_prio = {p: n for p, n in q(f"SELECT prio,COUNT(*) FROM egc WHERE t>=?{sc} GROUP BY prio", floor, *sa)}
        by_service = [{"service": s, "n": n} for s, n in q(
            f"SELECT service,COUNT(*) FROM egc WHERE t>=?{sc} GROUP BY service ORDER BY 2 DESC", floor, *sa)]
        hourly = {h: {"hour": h} for h in range(floor, hmax + 1, 3600)}
        for h, p, n in q(f"SELECT t/3600*3600,prio,COUNT(*) FROM egc WHERE t>=?{sc} GROUP BY 1,2", floor, *sa):
            hourly.setdefault(h, {"hour": h})[p] = n
        pc, pa = ("", ()) if not prio or prio == "all" else (" AND prio=?", (prio,))
        rows = [{"t": t, "sat": s, "station": st, "service": sv, "prio": p, "id": i, "parts": pt, "complete": c, "rep": r,
                 "txt": x} for t, s, st, sv, p, i, pt, c, r, x in q(
            f"SELECT t,sat,station,service,prio,egc_id,parts,complete,rep,txt FROM egc WHERE t>=?{sc}{pc} "
            f"ORDER BY t DESC LIMIT 300", floor, *sa, *pa)]
        stations = [{"sat": s, "freq": f, "les": l, "station": st, "t": t} for s, f, l, st, t in q(
            f"SELECT sat,freq,les,station,t FROM stations WHERE 1=1{sc}", *sa)]
        chans = [{"sat": s, "freq": f, "frames": fr, "ok": ok, "bad": bad, "sync_pct": 100.0 * sy / m if m else None}
                 for s, f, fr, ok, bad, sy, m in q(
            f"SELECT sat,freq,SUM(frames),SUM(ok),SUM(bad),SUM(sync_minutes),SUM(minutes) FROM chan_hourly "
            f"WHERE svc='stdc' AND hour>=?{sc} GROUP BY sat,freq", floor, *sa)]
        return {"satellites": satellites_meta(), "sats_present": present, "floor": floor, "hmax": hmax,
                "by_prio": by_prio, "by_service": by_service, "hourly": [hourly[h] for h in sorted(hourly)],
                "rows": rows, "stations": stations, "channels": chans,
                "last_data": q("SELECT MAX(t) FROM egc")[0][0]}
    finally:
        con.close()


BODY = """
  <div class="cards" id="cards"></div>
  <div class="grid">
    <div class="panel">
      <h2>EGC broadcasts per hour <span class="hint">by priority</span></h2>
      <div class="legend" id="leg"></div><div id="hourly"></div>
    </div>
    <div class="panel">
      <h2>STD-C channels <span class="hint">NCS carriers in window; the station from the bulletin board</span></h2>
      <div id="chans"></div>
    </div>
  </div>
  <div class="panel">
    <h2>Messages <span class="hint">newest first; [&hellip;] marks parts not received</span></h2>
    <div class="controls" id="prio" style="margin-bottom:8px"></div>
    <div class="scroll" id="rows" style="max-height:900px"></div>
  </div>
  <div class="panel">
    <h2>Services <span class="hint">in window</span></h2>
    <div id="services"></div>
  </div>
"""

SCRIPT = r"""
let PRIO="all";
EXTRA=()=>"&prio="+PRIO;
const PCOL={Routine:"#8b98a5",Safety:"#d29922",Urgency:"#f778ba",Distress:"#ff7b8b"},PKEYS=["Routine","Safety","Urgency","Distress"];
function render(D){
  const p=D.by_prio,tot=PKEYS.reduce((a,k)=>a+(p[k]||0),0);
  $("cards").innerHTML=[card("EGC messages",fmt(tot)),card("Distress",'<span style="color:#ff7b8b">'+fmt(p.Distress||0)+'</span>'),
    card("Urgency",'<span class="hotc">'+fmt(p.Urgency||0)+'</span>'),card("Safety",'<span class="warnc">'+fmt(p.Safety||0)+'</span>'),
    card("Routine",fmt(p.Routine||0)),card("NCS stations",D.stations.length)].join("");
  legend($("leg"),PKEYS,PCOL,{Routine:"Routine",Safety:"Safety",Urgency:"Urgency",Distress:"Distress"});
  bars($("hourly"),D.hourly,PKEYS,PCOL,170);
  $("chans").innerHTML=D.channels.length?'<table><tr><th>Satellite</th><th>MHz</th><th>NCS</th><th class="r">In sync</th><th class="r">Frames</th><th class="r">Packets OK</th></tr>'+
    D.channels.map(c=>{const st=D.stations.find(s=>s.sat===c.sat&&s.freq===c.freq),tot=c.ok+c.bad;
      return '<tr><td>'+esc(satName(c.sat))+'</td><td class="mono">'+mhz(c.freq)+'</td><td>'+esc(st?st.station:"–")+'</td><td class="r">'+
        (c.sync_pct==null?"–":c.sync_pct.toFixed(0)+"%")+'</td><td class="r n">'+fmt(c.frames)+'</td><td class="r">'+(tot?(100*c.ok/tot).toFixed(1)+"%":"–")+'</td></tr>';}).join("")+'</table>'
    :'<div class="empty">no STD-C channel in this window — add one with -c FREQ:stdc or a plan that has one</div>';
  $("prio").innerHTML=["all",...PKEYS].map(k=>'<button data-v="'+k+'"'+(k===PRIO?' class="on"':'')+'>'+(k==="all"?"All":k)+'</button>').join("");
  $("prio").querySelectorAll("button").forEach(b=>b.onclick=()=>{PRIO=b.dataset.v;load();});
  $("rows").innerHTML=D.rows.length?'<table><tr><th>UTC</th><th>Station</th><th>Priority</th><th>Service</th><th>Text</th></tr>'+
    D.rows.map(m=>'<tr><td class="mono n">'+dt(m.t)+'</td><td>'+esc(m.station||satName(m.sat))+'</td><td><span class="prio '+esc(m.prio)+'">'+esc(m.prio)+
      '</span></td><td>'+esc(m.service)+'<br><span class="n">#'+m.id+(m.complete?'':' · <span class="warnc">incomplete</span>')+'</span></td><td class="body mono">'+
      esc(m.txt)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">no EGC broadcasts in this window</div>';
  const mx=Math.max(1,...D.by_service.map(s=>s.n));
  $("services").innerHTML=D.by_service.length?'<table>'+D.by_service.map(s=>'<tr><td style="width:1%">'+esc(s.service)+
    '</td><td><span class="bar" style="width:'+Math.max(2,Math.round(300*s.n/mx))+'px;background:var(--c)"></span><span class="n">'+s.n+'</span></td></tr>').join("")+'</table>'
    :'<div class="empty">none</div>';
}
"""

PAGE = page("Inmarsat Observatory · Maritime", "maritime", "STD-C EGC: SafetyNET and FleetNET", BODY, SCRIPT,
            "/api/maritime")
