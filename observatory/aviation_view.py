# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inmarsat Observatory — Aviation: every aircraft heard, each one's message history across all
satellites and services, and the CPDLC and ADS-C logs. Adapted from vdl2-observatory's Messages
page."""

from common import connect_ro, db_ready, les_names, page, sat_clause, satellites_meta, window


def build_aviation(path, hours=None, sat=None, aes=None, limit=300):
    err = db_ready(path)
    if err:
        return err
    con = connect_ro(path)
    try:
        floor, hmax = window(con, hours)
        if floor is None:
            return {"error": "no data yet — run inmarsat-rx with --archive, then rollup.py"}
        t0 = floor
        sc, sa = sat_clause(sat)
        q = lambda sql, *a: con.execute(sql, a).fetchall()
        present = [r[0] for r in q("SELECT DISTINCT sat FROM msg_hourly")]
        cards = {
            "messages": q(f"SELECT COUNT(*) FROM messages WHERE t>=? AND ack=0{sc}", t0, *sa)[0][0],
            "aircraft": q(f"SELECT COUNT(DISTINCT aes) FROM messages WHERE t>=?{sc}", t0, *sa)[0][0],
            "tails": q(f"SELECT COUNT(DISTINCT reg) FROM messages WHERE t>=? AND reg<>''{sc}", t0, *sa)[0][0],
            "cpdlc": q(f"SELECT COUNT(*) FROM cpdlc_log WHERE t>=?{sc}", t0, *sa)[0][0],
            "adsc": q(f"SELECT COUNT(*) FROM adsc_log WHERE t>=?{sc}", t0, *sa)[0][0],
            "cpdlc_aircraft": q(f"SELECT COUNT(DISTINCT aes) FROM cpdlc_log WHERE t>=?{sc}", t0, *sa)[0][0],
        }
        tails = [{"aes": a, "reg": r, "n": n, "acks": k, "last": last, "svcs": s, "sats": st, "cpdlc": c, "adsc": d}
                 for a, r, n, k, last, s, st, c, d in q(
                     f"SELECT aes,MAX(reg),COUNT(*),SUM(ack),MAX(t),GROUP_CONCAT(DISTINCT svc),GROUP_CONCAT(DISTINCT sat),"
                     f"SUM(app='CPDLC'),SUM(app='ADS-C') FROM messages WHERE t>=?{sc} GROUP BY aes "
                     f"ORDER BY MAX(t) DESC LIMIT 1000", t0, *sa)]
        history = []
        if aes:
            history = [{"t": t, "sat": s, "svc": v, "freq": f, "ges": g, "reg": r, "lbl": l, "blk": b, "txt": x,
                        "ack": k, "app": ap, "app_sum": su}
                       for t, s, v, f, g, r, l, b, x, k, ap, su in q(
                           "SELECT t,sat,svc,freq,ges,reg,lbl,blk,txt,ack,app,app_sum FROM messages WHERE aes=? "
                           "ORDER BY t DESC LIMIT ?", aes, limit)]
        cpdlc = [{"t": t, "aes": a, "reg": r, "ges": g, "svc": v, "sat": s, "txt": x, "summary": su}
                 for t, a, r, g, v, s, x, su in q(
                     f"SELECT t,aes,reg,ges,svc,sat,txt,summary FROM cpdlc_log WHERE t>=?{sc} ORDER BY t DESC LIMIT 200",
                     t0, *sa)]
        adsc = [{"t": t, "aes": a, "reg": r, "ges": g, "svc": v, "sat": s, "kinds": k, "summary": su}
                for t, a, r, g, v, s, k, su in q(
                    f"SELECT t,aes,reg,ges,svc,sat,kinds,summary FROM adsc_log WHERE t>=?{sc} ORDER BY t DESC LIMIT 200",
                    t0, *sa)]
        labels = [{"lbl": l, "n": n} for l, n in q(
            f"SELECT lbl,COUNT(*) FROM messages WHERE t>=? AND ack=0{sc} GROUP BY lbl ORDER BY 2 DESC LIMIT 16", t0, *sa)]
        return {"satellites": satellites_meta(), "sats_present": present, "floor": floor, "hmax": hmax,
                "les_names": les_names(path), "cards": cards, "tails": tails, "aes": aes, "history": history, "cpdlc": cpdlc, "adsc": adsc,
                "labels": labels, "last_data": q("SELECT MAX(t) FROM messages")[0][0]}
    finally:
        con.close()


BODY = """
  <div class="cards" id="cards"></div>
  <div class="grid">
    <div class="panel">
      <h2>Aircraft <span class="hint">in window, latest first · click one for its history</span></h2>
      <input class="search" id="q" placeholder="tail or AES" autocomplete="off">
      <div class="scroll" id="tails"></div>
    </div>
    <div class="panel">
      <h2 id="histh">History <span class="hint">choose an aircraft</span></h2>
      <div class="scroll" id="hist"><div class="empty">every message to the aircraft you choose, on every satellite and service</div></div>
    </div>
  </div>
  <div class="panel">
    <h2>CPDLC <span class="hint">controller–pilot data link uplinks decoded by libacars; newest first</span></h2>
    <div class="scroll" id="cpdlc"></div>
  </div>
  <div class="grid">
    <div class="panel">
      <h2>ADS-C <span class="hint">contract requests to aircraft; their reports go back on the R/T channels</span></h2>
      <div class="scroll" id="adsc"></div>
    </div>
    <div class="panel">
      <h2>Labels <span class="hint">most common ACARS labels in window, acknowledgements left out</span></h2>
      <div id="labels"></div>
    </div>
  </div>
"""

SCRIPT = r"""
let SEL=decodeURIComponent(location.hash.slice(1))||null;
EXTRA=()=>SEL?"&aes="+encodeURIComponent(SEL):"";
window.addEventListener("hashchange",()=>{SEL=decodeURIComponent(location.hash.slice(1))||null;load();});
$("q").oninput=()=>render(D);
function name(a){return a.reg||a.aes;}
function render(D){
  const c=D.cards;
  $("cards").innerHTML=[card("Messages",fmt(c.messages),"not acks"),card("Aircraft",fmt(c.aircraft),fmt(c.tails)+" with a tail"),
    card("CPDLC",'<span class="hotc">'+fmt(c.cpdlc)+'</span>',fmt(c.cpdlc_aircraft)+" aircraft"),card("ADS-C",fmt(c.adsc),"contract messages")].join("");
  const f=$("q").value.trim().toUpperCase();
  const T=D.tails.filter(a=>!f||(a.reg||"").toUpperCase().includes(f)||a.aes.includes(f));
  $("tails").innerHTML=T.length?'<table><tr><th>Tail</th><th>AES</th><th>Heard on</th><th class="r">Msgs</th><th class="r">Acks</th><th class="r">Last</th></tr>'+
    T.slice(0,400).map(a=>'<tr class="click'+(a.aes===SEL?" sel":"")+'" data-aes="'+esc(a.aes)+'"><td class="mono">'+esc(a.reg||"—")+
      (a.cpdlc?' <span class="hotc" title="CPDLC">◆</span>':'')+'</td><td class="mono n">'+esc(a.aes)+'</td><td>'+
      a.svcs.split(",").sort((x,y)=>ORDER.indexOf(x)-ORDER.indexOf(y)).map(svc).join(" ")+'</td><td class="r">'+(a.n-a.acks)+
      '</td><td class="r n">'+a.acks+'</td><td class="r n">'+dt(a.last).slice(6)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">no aircraft match</div>';
  $("tails").querySelectorAll("tr.click").forEach(r=>r.onclick=()=>{location.hash=r.dataset.aes;});
  if(SEL){const a=D.tails.find(x=>x.aes===SEL);
    $("histh").innerHTML='History · '+esc(a?name(a):SEL)+' <span class="hint">AES '+esc(SEL)+' · all time, newest first</span>';
    $("hist").innerHTML=D.history.length?'<table><tr><th>UTC</th><th>Sat</th><th>Service</th><th>GES</th><th>Lbl</th><th>Text</th></tr>'+
      D.history.map(m=>'<tr><td class="mono n">'+dt(m.t)+'</td><td>'+esc(satName(m.sat))+'</td><td>'+svc(m.svc)+'</td><td class="mono n">'+esc(m.ges)+
        '</td><td class="mono">'+esc(m.lbl)+'</td><td class="body mono">'+(m.app?'<span class="app'+(m.app==="ADS-C"?" adsc":"")+'">'+esc(m.app)+'</span>'+esc(m.app_sum)+'<br>':'')+
        (m.txt?esc(m.txt):'<span class="n">'+(m.ack?"acknowledgement":"—")+'</span>')+'</td></tr>').join("")+'</table>'
      :'<div class="empty">no messages</div>';}
  $("cpdlc").innerHTML=D.cpdlc.length?'<table><tr><th>UTC</th><th>Tail</th><th>GES</th><th>Service</th><th>Elements</th><th>Text</th></tr>'+
    D.cpdlc.map(m=>'<tr><td class="mono n">'+dt(m.t)+'</td><td class="mono"><a style="color:inherit" href="#'+esc(m.aes)+'">'+esc(m.reg||m.aes)+
      '</a></td><td class="mono n">'+esc(m.ges)+'</td><td>'+svc(m.svc)+'</td><td class="body">'+esc(m.summary)+'</td><td class="body mono n">'+esc(m.txt)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">no CPDLC in this window</div>';
  $("adsc").innerHTML=D.adsc.length?'<table><tr><th>UTC</th><th>Tail</th><th>GES</th><th>Service</th><th>Contract</th></tr>'+
    D.adsc.map(m=>'<tr><td class="mono n">'+dt(m.t)+'</td><td class="mono"><a style="color:inherit" href="#'+esc(m.aes)+'">'+esc(m.reg||m.aes)+
      '</a></td><td class="mono n">'+esc(m.ges)+'</td><td>'+svc(m.svc)+'</td><td class="body">'+esc(m.summary)+'</td></tr>').join("")+'</table>'
    :'<div class="empty">no ADS-C in this window</div>';
  const mx=Math.max(1,...D.labels.map(l=>l.n));
  $("labels").innerHTML=D.labels.length?'<table>'+D.labels.map(l=>'<tr><td class="mono" style="width:1%">'+esc(l.lbl)+
    '</td><td><span class="bar" style="width:'+Math.max(2,Math.round(260*l.n/mx))+'px;background:var(--accent)"></span><span class="n">'+fmt(l.n)+'</span></td></tr>').join("")+'</table>'
    :'<div class="empty">none</div>';
}
"""

PAGE = page("Inmarsat Observatory · Aviation", "aviation", "ACARS, CPDLC and ADS-C over Inmarsat Aero", BODY, SCRIPT,
            "/api/aviation")
