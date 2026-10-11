// Self-contained dashboard served at http://<esp-ip>/.
//
// No CDN, no external fonts, no filesystem image: the motorhome has no
// internet, so everything the page needs must be in this string. Served with
// send_P() straight out of flash.
#pragma once

#include <Arduino.h>

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DALY BMS</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#16181d;--mut:#6b7280;--line:#e3e6ea;
--ok:#12a150;--warn:#d97706;--bad:#dc2626;--accent:#2563eb}
@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--card:#171a20;--fg:#e8eaed;
--mut:#9aa1ab;--line:#272b33;--ok:#2bbf6a;--warn:#f0a13a;--bad:#f2555a;--accent:#5b8dff}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);
font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
h1{font-size:17px;margin:0 0 12px;display:flex;align-items:center;gap:8px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--mut);flex:none}
.wrap{max-width:760px;margin:0 auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;
padding:14px;margin-bottom:12px}
#banner{display:none;background:var(--bad);color:#fff;border:0;font-weight:600}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(140px,1fr))}
.stat .k{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.04em}
.stat .v{font-size:24px;font-weight:650;font-variant-numeric:tabular-nums;margin-top:2px}
.stat .u{font-size:14px;font-weight:400;color:var(--mut)}
.soc{display:flex;align-items:center;gap:16px}
.soc svg{flex:none}
.cells{display:grid;gap:5px;grid-template-columns:repeat(auto-fit,minmax(76px,1fr))}
.cell{border:1px solid var(--line);border-radius:7px;padding:6px 8px;font-size:13px;
font-variant-numeric:tabular-nums}
.cell b{display:block;font-size:11px;color:var(--mut);font-weight:500}
.cell.hi{border-color:var(--bad)}.cell.lo{border-color:var(--accent)}
.row{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid var(--line)}
.row:last-child{border:0}.row span:first-child{color:var(--mut)}
.pill{padding:1px 8px;border-radius:99px;font-size:12px;font-weight:600;
background:var(--line);color:var(--mut)}
.pill.on{background:var(--ok);color:#fff}.pill.off{background:var(--bad);color:#fff}
footer{color:var(--mut);font-size:12px;text-align:center;margin-top:14px}
footer a{color:var(--mut)}
</style></head><body><div class="wrap">

<h1><span class="dot" id="dot"></span>DALY BMS</h1>
<div class="card" id="banner"></div>

<div class="card soc">
  <svg width="104" height="104" viewBox="0 0 104 104">
    <circle cx="52" cy="52" r="44" fill="none" stroke="var(--line)" stroke-width="12"/>
    <circle id="arc" cx="52" cy="52" r="44" fill="none" stroke="var(--ok)" stroke-width="12"
      stroke-linecap="round" stroke-dasharray="0 999" transform="rotate(-90 52 52)"/>
    <text id="socTxt" x="52" y="52" text-anchor="middle" dominant-baseline="central"
      font-size="21" font-weight="700" fill="var(--fg)">--</text>
  </svg>
  <div class="grid" style="flex:1">
    <div class="stat"><div class="k">Voltage</div><div class="v"><span id="v">--</span><span class="u"> V</span></div></div>
    <div class="stat"><div class="k">Current</div><div class="v"><span id="a">--</span><span class="u"> A</span></div></div>
    <div class="stat"><div class="k">Power</div><div class="v"><span id="w">--</span><span class="u"> W</span></div></div>
  </div>
</div>

<div class="card">
  <div class="row"><span>State</span><b id="state">--</b></div>
  <div class="row"><span>Charge MOS</span><span class="pill" id="cmos">--</span></div>
  <div class="row"><span>Discharge MOS</span><span class="pill" id="dmos">--</span></div>
  <div class="row"><span>Remaining</span><b id="rem">--</b></div>
  <div class="row"><span id="etaK">Time left</span><b id="eta">--</b></div>
  <div class="row"><span>Cycles</span><b id="cyc">--</b></div>
  <div class="row"><span>Temperature</span><b id="temp">--</b></div>
</div>

<div class="card">
  <div class="row"><span>Highest cell</span><b id="cmax">--</b></div>
  <div class="row"><span>Lowest cell</span><b id="cmin">--</b></div>
  <div class="row"><span>Charger / load detected</span><b id="chl">--</b></div>
</div>

<div class="card">
  <div class="row"><span>Cells</span><b id="cellinfo">--</b></div>
  <div class="cells" id="cells" style="margin-top:10px"></div>
</div>

<div class="card">
  <div class="row"><span>Power mode</span><b id="mode">--</b></div>
  <div class="row"><span>Low-voltage guard</span><span class="pill" id="guard">--</span></div>
  <div class="row"><span>Guard low for</span><b id="glow">--</b></div>
  <div class="row"><span>WiFi</span><b id="wifi">--</b></div>
  <div class="row"><span>MQTT</span><span class="pill" id="mqtt">--</span></div>
  <div class="row"><span>MQTT server</span><b id="mqttsrv">--</b></div>
  <div class="row"><span>Free heap</span><b id="heap">--</b></div>
  <div class="row"><span>BMS polls</span><b id="sweeps">--</b></div>
</div>

<footer>
  <span id="meta">connecting...</span> &middot; <a href="/log">log</a>
</footer>
</div><script>
var $=function(i){return document.getElementById(i)};
function fmt(n,d){return (n==null)?'--':n.toFixed(d===undefined?1:d)}

function render(s){
  var stale=!s.valid;
  $('dot').style.background=stale?'var(--bad)':'var(--ok)';
  var b=$('banner');
  if(stale){
    b.style.display='block';
    b.textContent=s.stale_seconds>=65535
      ? 'No data from BMS since boot - check wiring and baud rate.'
      : 'Data is stale: no valid BMS frame for '+s.stale_seconds+' s.';
  } else b.style.display='none';

  var soc=s.pack.soc;
  $('socTxt').textContent=fmt(soc,0)+'%';
  var C=2*Math.PI*44;
  $('arc').setAttribute('stroke-dasharray',(C*Math.max(0,Math.min(100,soc))/100)+' '+C);
  $('arc').setAttribute('stroke',soc<20?'var(--bad)':soc<40?'var(--warn)':'var(--ok)');

  $('v').textContent=fmt(s.pack.voltage,2);
  $('a').textContent=fmt(s.pack.current,1);
  $('w').textContent=fmt(s.pack.power,0);

  $('state').textContent=s.status.state;
  [['cmos',s.status.charge_mos],['dmos',s.status.discharge_mos]].forEach(function(p){
    var e=$(p[0]);e.textContent=p[1]?'ON':'OFF';e.className='pill '+(p[1]?'on':'off')});
  $('rem').textContent=(s.status.remaining_mah/1000).toFixed(1)+' Ah';
  $('cyc').textContent=s.status.cycles;
  // Time to empty at the present draw. The BMS does not report full capacity,
  // so there is no time-to-full; meaningless near zero current.
  var amps=s.pack.current,rem=s.status.remaining_mah/1000,eta='--',etaK='Time left';
  if(amps<=-0.5){var hrs=rem/-amps;eta=hrs<1?Math.round(hrs*60)+' min':hrs.toFixed(1)+' h'}
  else if(amps>=0.5)eta='charging';
  $('etaK').textContent=etaK;$('eta').textContent=eta;
  $('cmax').textContent=s.cells.max_mv?(s.cells.max_mv/1000).toFixed(3)+' V (cell '+s.cells.max_idx+')':'--';
  $('cmin').textContent=s.cells.min_mv?(s.cells.min_mv/1000).toFixed(3)+' V (cell '+s.cells.min_idx+')':'--';
  $('chl').textContent=(s.status.charger_present?'charger':'no charger')+' / '+(s.status.load_present?'load':'no load');
  $('temp').textContent=s.temps.count?(s.temps.min+' to '+s.temps.max+' °C'):'--';

  $('cellinfo').textContent=s.cells.count
    ? s.cells.count+' cells · delta '+s.cells.delta_mv+' mV'
    : '--';
  var h='';
  for(var i=0;i<s.cells.mv.length;i++){
    var mv=s.cells.mv[i],n=i+1,c='cell';
    if(n==s.cells.max_idx)c+=' hi'; if(n==s.cells.min_idx)c+=' lo';
    h+='<div class="'+c+'"><b>Cell '+n+'</b>'+(mv/1000).toFixed(3)+' V</div>';
  }
  $('cells').innerHTML=h||'<span style="color:var(--mut)">no cell data yet</span>';

  $('mode').textContent=s.mode;
  var g=$('guard');g.textContent=s.guard.state;
  g.className='pill '+(s.guard.state=='counting'?'off':s.guard.state=='normal'?'on':'');
  $('glow').textContent=s.guard.low_for_s+' s'+(s.guard.sleeps?' · '+s.guard.sleeps+' sleeps':'');
  $('wifi').textContent=s.wifi.ssid+' · '+s.wifi.rssi+' dBm · '+s.ip;
  var m=$('mqtt');m.textContent=s.mqtt.connected?'connected':'disconnected';
  m.className='pill '+(s.mqtt.connected?'on':'off');
  $('mqttsrv').textContent=s.mqtt.server+(s.mqtt.host?' · '+s.mqtt.host:'');
  $('heap').textContent=(s.stats.free_heap/1024).toFixed(1)+' kB';
  $('sweeps').textContent=s.stats.sweeps;

  $('meta').textContent='up '+Math.floor(s.uptime_s/60)+' min · RSSI '+s.rssi
    +' dBm · ok '+s.stats.frames_ok+' · timeouts '+s.stats.timeouts
    +' · crc '+s.stats.checksum_errors;
}

// One request at a time, the next only after the last has finished. The ESP has
// very little spare memory once TLS is up, and in normal mode its answers take
// seconds: a fixed timer would stack up requests until it runs out and resets.
var every=2000;
function poll(){
  fetch('/status.json',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(s){render(s);every=(s.mode=='high')?2000:10000})
    .catch(function(){
      $('dot').style.background='var(--bad)';
      $('meta').textContent='cannot reach the ESP';
      every=10000;
    })
    .then(function(){setTimeout(poll,every)});
}
poll();
</script></body></html>)HTML";
