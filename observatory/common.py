# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Inmarsat Observatory — what every page shares: the satellites, the database helpers, and the
page shell (style, header with navigation, time window and satellite selector, and the small JS
library the pages draw with). The look is vdl2-observatory's and iridium-observatory's.
"""

import os
import sqlite3

# Kept in step with inmarsat-rx's src/plans.c.
SATELLITES = {
    "i4f3": {"name": "I-4 F3", "slot": "98°W", "region": "Americas"},
    "i3f5": {"name": "I-3 F5", "slot": "54°W", "region": "Atlantic"},
    "alphasat": {"name": "Alphasat", "slot": "25°E", "region": "Europe, Africa"},
}
SERVICES = {"aero": "Aero-H+ 10.5k", "aerol600": "Aero-L 600", "aerol1200": "Aero-L 1200", "stdc": "STD-C"}


def connect_ro(path):
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=5)


def window(con, hours, table="msg_hourly", col="hour"):
    """(floor, hmax) hour bounds: the last `hours` hours of data (all when 0), counted back from
    the newest hour in the database rather than the clock, so a replayed recording shows."""
    r = con.execute(f"SELECT MIN({col}), MAX({col}) FROM {table}").fetchone()
    hmin, hmax = r
    if hmin is None:
        return None, None
    hours = int(hours) if hours else 0
    floor = max(hmin, hmax - (hours - 1) * 3600) if hours > 0 else hmin
    return floor, hmax


def sat_clause(sat, col="sat"):
    """SQL fragment and args restricting to one satellite (sat None or 'all': no restriction)."""
    if not sat or sat == "all":
        return "", ()
    return f" AND {col}=?", (sat,)


def satellites_meta():
    return [{"id": k, **v} for k, v in SATELLITES.items()]


STYLE = r"""
  :root{--bg:#0e1116;--panel:#161b22;--panel2:#1c232c;--line:#2b333d;--grid:#242c36;
    --fg:#e6edf3;--dim:#8b98a5;--accent:#4aa8ff;--good:#3fb950;--warn:#d29922;--hot:#f778ba;
    --h:#4aa8ff;--l6:#3fb950;--l12:#a371f7;--c:#d29922}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
  header{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap;padding:14px 22px;border-bottom:1px solid var(--line);background:var(--panel)}
  header h1{font-size:18px;margin:0;font-weight:650}
  header .sub{color:var(--dim);font-size:12.5px}
  header .spacer{flex:1}
  .nav{display:flex;gap:4px}
  .nav a{color:var(--dim);text-decoration:none;font-size:13px;padding:4px 11px;border-radius:6px;border:1px solid transparent}
  .nav a:hover{color:var(--fg)}
  .nav a.on{color:var(--fg);background:var(--panel2);border-color:var(--line)}
  .nav a[hidden]{display:none}
  .controls{display:flex;gap:4px}
  .controls button{background:transparent;color:var(--dim);border:1px solid var(--line);border-radius:6px;padding:3px 10px;font:inherit;font-size:12px;cursor:pointer}
  .controls button:hover{color:var(--fg)}
  .controls button.on{color:var(--fg);background:var(--panel2);border-color:var(--accent)}
  .status{color:var(--dim);font-size:12px}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--good);margin-right:5px;vertical-align:middle}
  main{padding:16px 22px;max-width:1500px;margin:0 auto}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px;margin-bottom:14px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
  .card .k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.6px}
  .card .v{font-size:24px;font-weight:600;margin-top:3px}
  .card .v small{font-size:12.5px;color:var(--dim);font-weight:400}
  .panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin-bottom:14px;min-width:0}
  .panel h2{font-size:13px;margin:0 0 8px;font-weight:600;display:flex;justify-content:space-between;gap:8px;flex-wrap:wrap}
  .panel h2 .hint{color:var(--dim);font-weight:400;font-size:11.5px}
  .panel h3{font-size:12.5px;margin:14px 0 4px;font-weight:600;display:flex;gap:10px;align-items:baseline;flex-wrap:wrap}
  .panel h3:first-of-type{margin-top:2px}
  .panel h3 .hint{color:var(--dim);font-weight:400;font-size:11.5px}
  .grid{display:grid;grid-template-columns:minmax(0,3fr) minmax(0,2fr);gap:14px}
  .grid2{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:14px}
  @media(max-width:1000px){.grid,.grid2{grid-template-columns:minmax(0,1fr)}}
  .scroll{max-height:520px;overflow:auto}
  table{width:100%;border-collapse:collapse;font-size:12.5px}
  th{text-align:left;color:var(--dim);font-weight:500;font-size:11px;text-transform:uppercase;letter-spacing:.5px;padding:5px 7px;border-bottom:1px solid var(--line);position:sticky;top:0;background:var(--panel)}
  th.r,td.r{text-align:right}
  td{padding:5px 7px;border-bottom:1px solid #1b2129;vertical-align:top;white-space:nowrap}
  td.body{white-space:normal;overflow-wrap:anywhere;min-width:240px}
  tr.sel td{background:#1d2b3c}
  tr.click{cursor:pointer}tr.click:hover td{background:var(--panel2)}
  .mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
  .n{color:var(--dim);font-variant-numeric:tabular-nums}
  .ok{color:var(--good)}.warnc{color:var(--warn)}.hotc{color:var(--hot)}
  .svc{font-size:10.5px;padding:1px 6px;border-radius:4px;border:1px solid}
  .svc.aero{color:var(--h);border-color:#1f4a73}.svc.aerol600{color:var(--l6);border-color:#1f4f2b}
  .svc.aerol1200{color:var(--l12);border-color:#43306b}.svc.stdc{color:var(--c);border-color:#5c4513}
  .app{font-size:10.5px;padding:1px 5px;border-radius:4px;background:#2a1830;color:var(--hot);margin-right:5px}
  .app.adsc{background:#2b2410;color:var(--warn)}
  .prio{font-size:10.5px;padding:1px 5px;border-radius:4px;background:var(--panel2);color:var(--dim)}
  .prio.Safety{color:var(--warn)}.prio.Urgency{color:var(--hot)}.prio.Distress{background:#3d1520;color:#ff7b8b}
  .bar{display:inline-block;height:6px;border-radius:3px;vertical-align:middle;margin-right:6px}
  .legend{display:flex;gap:14px;flex-wrap:wrap;font-size:11.5px;color:var(--dim);margin:2px 0 6px}
  .legend i{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:5px;vertical-align:middle}
  .empty{color:var(--dim);padding:20px;text-align:center;font-size:13px}
  .gl{stroke:var(--grid);stroke-width:1}
  .ax{fill:var(--dim);font-size:11px}
  input.search{background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:4px 8px;font:inherit;font-size:12.5px;width:100%;margin-bottom:6px}
  pre.dec{white-space:pre-wrap;font-size:11.5px;color:#c9d1d9;background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:8px;margin:6px 0 0;max-height:300px;overflow:auto}
"""

# Shared JS: formatting, the window/satellite controls (in the URL hash and remembered between
# pages), stacked hourly bars, and the fetch loop. Each page defines render(D).
SCRIPT = r"""
const $=id=>document.getElementById(id);
const SVC={aero:"Aero-H+ 10.5k",aerol600:"Aero-L 600",aerol1200:"Aero-L 1200",stdc:"STD-C"};
const SVCCOL={aero:"#4aa8ff",aerol600:"#3fb950",aerol1200:"#a371f7",stdc:"#d29922"};
const ORDER=["stdc","aero","aerol1200","aerol600"];
function esc(s){return (s==null?"":""+s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));}
function fmt(n){n=n||0;return n>=1e4?(n/1e3).toFixed(1)+"k":""+n;}
function pad(x){return String(x).padStart(2,"0");}
function dt(t){const d=new Date(t*1000);return pad(d.getUTCMonth()+1)+"-"+pad(d.getUTCDate())+" "+pad(d.getUTCHours())+":"+pad(d.getUTCMinutes())+":"+pad(d.getUTCSeconds());}
function hm(t){const d=new Date(t*1000);return pad(d.getUTCHours())+":"+pad(d.getUTCMinutes());}
function mhz(f){return (f/1e6).toFixed(4);}
function card(k,v,sub){return '<div class="card"><div class="k">'+k+'</div><div class="v">'+v+(sub?' <small>'+sub+'</small>':'')+'</div></div>';}
function svc(t){return '<span class="svc '+t+'">'+(SVC[t]||esc(t))+'</span>';}
function el(tag,attrs,text){const e=document.createElementNS("http://www.w3.org/2000/svg",tag);
  for(const k in attrs||{})e.setAttribute(k,attrs[k]);if(text!=null)e.textContent=text;return e;}
let D=null,SATS=[];
const ST={hours:"24",sat:"all"};
try{Object.assign(ST,JSON.parse(localStorage.getItem("inmarsat-obs")||"{}"));}catch(e){}
function satName(id){const s=SATS.find(x=>x.id===id);return s?s.name:id;}
function saveST(){try{localStorage.setItem("inmarsat-obs",JSON.stringify(ST));}catch(e){}}
function controls(){
  document.querySelectorAll("#win button").forEach(b=>{b.classList.toggle("on",b.dataset.h===ST.hours);
    b.onclick=()=>{ST.hours=b.dataset.h;saveST();load();};});
  const present=(D&&D.sats_present)||[];
  $("sat").innerHTML=[["all","All satellites"],...present.map(s=>[s,satName(s)])].map(([v,t])=>
    '<button data-v="'+esc(v)+'"'+(v===ST.sat?' class="on"':'')+'>'+esc(t)+'</button>').join("");
  $("sat").querySelectorAll("button").forEach(b=>b.onclick=()=>{ST.sat=b.dataset.v;saveST();load();});
}
// Stacked bars per hour: rows [{hour, key:value...}], keys in stacking order.
function bars(host,rows,keys,colors,height){
  host.innerHTML="";if(!rows.length){host.innerHTML='<div class="empty">no data in this window</div>';return;}
  const W=host.clientWidth||900,H=height||200,L=42,B=20,T=6,s=el("svg",{width:W,height:H});
  // Axis to a multiple of 4 so the five ticks are whole numbers.
  const tot=r=>keys.reduce((a,k)=>a+(r[k]||0),0),max=Math.ceil(Math.max(1,...rows.map(tot))/4)*4;
  const bw=(W-L-4)/rows.length,y=v=>T+(H-T-B)*(1-v/max);
  for(let i=0;i<=4;i++){const v=max*i/4,yy=y(v);s.appendChild(el("line",{x1:L,x2:W,y1:yy,y2:yy,class:"gl"}));
    s.appendChild(el("text",{x:L-6,y:yy+4,"text-anchor":"end",class:"ax"},fmt(Math.round(v))));}
  const every=Math.max(1,Math.ceil(rows.length/14));
  rows.forEach((r,i)=>{let acc=0;const x=L+i*bw+Math.max(1,bw*0.12),w=Math.max(1,bw*0.76);
    for(const k of keys){const v=r[k]||0;if(!v)continue;const y0=y(acc+v),h=y(acc)-y0;acc+=v;
      const rc=el("rect",{x,y:y0,width:w,height:Math.max(0,h),fill:colors[k]});
      rc.appendChild(el("title",{},hm(r.hour)+" UTC · "+(SVC[k]||k)+": "+v));s.appendChild(rc);}
    if(i%every===0)s.appendChild(el("text",{x:x+w/2,y:H-5,"text-anchor":"middle",class:"ax"},hm(r.hour)));});
  host.appendChild(s);
}
function legend(host,keys,colors,labels){host.innerHTML=keys.map(k=>'<span><i style="background:'+colors[k]+'"></i>'+esc((labels||SVC)[k]||k)+'</span>').join("");}
function samehost(port){const h=location.hostname;return location.protocol+"//"+(h.includes(":")?"["+h+"]":h)+":"+port+"/";}
async function load(){
  controls();
  try{const r=await fetch(API+"?hours="+ST.hours+"&sat="+encodeURIComponent(ST.sat)+EXTRA(),{cache:"no-store"});D=await r.json();}
  catch(e){$("status").innerHTML='<span class="dot" style="background:var(--warn)"></span>server not answering';return;}
  if(D.error){$("status").innerHTML='<span class="dot" style="background:var(--warn)"></span>'+esc(D.error);return;}
  SATS=D.satellites||[];
  if(ST.sat!=="all"&&!(D.sats_present||[]).includes(ST.sat)){ST.sat="all";}
  controls();
  const ll=$("livelink");if(LIVE_PORT){ll.href=samehost(LIVE_PORT);ll.hidden=false;}
  $("status").innerHTML='<span class="dot"></span>updated '+new Date().toLocaleTimeString()+
    (D.last_data?' · data to '+dt(D.last_data)+' UTC':'');
  render(D);
}
let EXTRA=()=>"";
"""


def page(title, active, sub, body, script, api):
    """A whole page: header (nav, window, satellite selector, status), body, shared + page JS."""
    on = ' class="on"'
    nav = "".join(
        f'<a href="{href}"{on if key == active else ""}>{label}</a>'
        for key, href, label in (("overview", "/", "Overview"), ("aviation", "/aviation", "Aviation"),
                                 ("maritime", "/maritime", "Maritime")))
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title><style>{STYLE}</style></head>
<body>
<header>
  <h1>Inmarsat Observatory</h1>
  <nav class="nav">{nav}<a id="livelink" href="#" hidden>Live</a></nav>
  <span class="sub">{sub}</span>
  <span class="spacer"></span>
  <div class="controls" id="sat"></div>
  <div class="controls" id="win">
    <button data-h="6">6h</button><button data-h="24">24h</button><button data-h="72">3d</button>
    <button data-h="168">7d</button><button data-h="0">all</button>
  </div>
  <span class="status" id="status"><span class="dot"></span>loading…</span>
</header>
<main>{body}</main>
<script>
const API="{api}",LIVE_PORT=__LIVE_PORT__;
{SCRIPT}
{script}
load();setInterval(load,60000);
</script>
</body></html>"""


def db_ready(path):
    if not os.path.exists(path):
        return {"error": "no database yet — run rollup.py first"}
    return None
