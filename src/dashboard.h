// Self-contained dashboard served at http://dalybms.local/.
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
.stat .sub{font-size:11px;color:var(--mut);font-variant-numeric:tabular-nums}
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
.pill.tog{cursor:pointer;border:0;font-family:inherit}.pill.tog:active{opacity:.7}
.pill.busy{opacity:.5}
details summary{cursor:pointer;color:var(--mut)}
.lt{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums;margin-bottom:6px}
.lt th{font-size:11px;color:var(--mut);font-weight:500;text-align:right;padding:3px 0}
.lt td{text-align:right;padding:4px 0;border-top:1px solid var(--line)}
.lt td:first-child{text-align:left;color:var(--mut)}
h3{font-size:13px;margin:14px 0 6px}
.fld4{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:8px}
.fld4 label{font-size:12px;color:var(--mut)}
.fld4 input,#pin{box-sizing:border-box;width:100%;padding:7px;border:1px solid var(--line);border-radius:7px;
background:var(--bg);color:var(--fg);font:inherit}
.btns{display:flex;gap:8px;margin-top:8px}
.btns button{flex:none;padding:4px 12px;border:1px solid var(--line);border-radius:7px;
background:var(--bg);color:var(--fg);font:inherit;font-size:13px}
.btns button:active{background:var(--line)}
@supports(background:color-mix(in srgb,red,blue)){
.btns button{background:color-mix(in srgb,var(--accent) 14%,var(--card));
border-color:color-mix(in srgb,var(--accent) 35%,var(--card))}
.btns button:active{background:color-mix(in srgb,var(--accent) 30%,var(--card))}}
dialog{border:1px solid var(--line);border-radius:12px;background:var(--card);color:var(--fg);padding:16px;width:min(280px,90vw)}
dialog::backdrop{background:rgba(0,0,0,.45)}
#lmsg{margin-top:8px;font-size:13px;color:var(--mut)}
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
    <div class="stat"><div class="k">Voltage</div><div class="v"><span id="v">--</span><span class="u"> V</span></div><div class="sub" id="vcell"></div><div class="sub" id="vlim"></div></div>
    <div class="stat"><div class="k">Current</div><div class="v"><span id="a">--</span><span class="u"> A</span></div></div>
    <div class="stat"><div class="k">Power</div><div class="v"><span id="w">--</span><span class="u"> W</span></div></div>
  </div>
</div>

<div class="card">
  <div class="row"><span>State</span><b id="state">--</b></div>
  <div class="row"><span>Charge MOS</span><button type="button" class="pill tog" id="cmos" onclick="toggleMos('charge_mos','cmos')" title="Click to switch (PIN)">--</button></div>
  <div class="row"><span>Discharge MOS</span><button type="button" class="pill tog" id="dmos" onclick="toggleMos('discharge_mos','dmos')" title="Click to switch (PIN)">--</button></div>
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
  <table class="lt">
    <tr><th></th><th>Cell high (V)</th><th>Cell low (V)</th><th>Pack high (V)</th><th>Pack low (V)</th></tr>
    <tr><td>L1</td><td id="lc0">--</td><td id="lc2">--</td><td id="lp0">--</td><td id="lp2">--</td></tr>
    <tr><td>L2</td><td id="lc1">--</td><td id="lc3">--</td><td id="lp1">--</td><td id="lp3">--</td></tr>
  </table>
  <div class="row"><span>Pack voltage profile</span><b id="prof">--</b></div>
  <div class="row"><span>BMS sleep timeout</span><b id="lsl">--</b></div>
  <div class="row"><span>Guard trips at</span><b id="gtrip">--</b></div>
</div>

<div class="card">
  <details id="lset"><summary>Change BMS settings</summary>
    <h3>Pack voltage profile</h3>
    <div class="btns">
      <button type="button" id="pb_normal" onclick="setProfile('normal')">Normal</button>
      <button type="button" id="pb_storage" onclick="setProfile('storage')">Storage</button>
    </div>
    <h3>Cell voltage limits (V)</h3>
    <div class="fld4">
      <label>High L1<input id="f_cell_high_l1_mv" inputmode="decimal"></label>
      <label>High L2<input id="f_cell_high_l2_mv" inputmode="decimal"></label>
      <label>Low L1<input id="f_cell_low_l1_mv" inputmode="decimal"></label>
      <label>Low L2<input id="f_cell_low_l2_mv" inputmode="decimal"></label>
    </div>
    <div class="btns"><button type="button" onclick="saveLimits('cell')">Save cell limits</button></div>
    <h3>Pack voltage limits (V)</h3>
    <div class="fld4">
      <label>High L1<input id="f_pack_high_l1_v" inputmode="decimal"></label>
      <label>High L2<input id="f_pack_high_l2_v" inputmode="decimal"></label>
      <label>Low L1<input id="f_pack_low_l1_v" inputmode="decimal"></label>
      <label>Low L2<input id="f_pack_low_l2_v" inputmode="decimal"></label>
    </div>
    <div class="btns"><button type="button" onclick="saveLimits('pack')">Save pack limits</button></div>
    <h3>BMS sleep</h3>
    <div class="fld4">
      <label>Timeout (s, min 10)<input id="f_sleep_timeout_s" inputmode="numeric"></label>
    </div>
    <div class="btns">
      <button type="button" onclick="saveLimits('sleep')">Save timeout</button>
      <button type="button" onclick="saveLimits('nosleep')">Turn sleep off</button>
    </div>
    <div id="lmsg"></div>
  </details>
</div>

<dialog id="pind"><form method="dialog" id="pinf">
  <div style="margin-bottom:8px" id="pinq">Enter PIN to change BMS settings</div>
  <input id="pin" type="password" inputmode="numeric" autocomplete="off">
  <div id="pinmsg" style="margin-top:8px;font-size:13px;color:var(--mut)"></div>
  <div class="btns"><button value="ok" id="pinok">OK</button><button value="cancel" id="pinx" formnovalidate>Cancel</button></div>
</form></dialog>

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
      : 'Data is stale: no pack reading from the BMS for'+s.stale_seconds+' s.';
  } else b.style.display='none';

  var soc=s.pack.soc;
  $('socTxt').textContent=fmt(soc,0)+'%';
  var C=2*Math.PI*44;
  $('arc').setAttribute('stroke-dasharray',(C*Math.max(0,Math.min(100,soc))/100)+' '+C);
  $('arc').setAttribute('stroke',soc<20?'var(--bad)':soc<40?'var(--warn)':'var(--ok)');

  $('v').textContent=fmt(s.pack.voltage,1);
  $('vcell').textContent=s.pack.cells_v?'cells '+fmt(s.pack.cells_v,2)+' V':'';
  $('a').textContent=fmt(s.pack.current,1);
  $('w').textContent=fmt(s.pack.power,0);

  $('state').textContent=s.status.state;
  [['cmos',s.status.charge_mos],['dmos',s.status.discharge_mos]].forEach(function(p){
    var e=$(p[0]);e.textContent=p[1]?'ON':'OFF';e.className='pill tog '+(p[1]?'on':'off');
    mosNow[p[0]]=!!p[1]});
  if(mosWant){
    if(mosNow[mosWant.id]==mosWant.on)mosEnd(mosWant.label+' is now '+(mosWant.on?'ON':'OFF'),true);
    else if(s.limits.write=='mismatch')mosEnd('The BMS did not switch it.',false);
    else if(Date.now()-mosWant.t>40000)mosEnd('No confirmation from the BMS yet.',false);
  }
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
  var L=s.limits;lastLimits=L;
  if(L.write&&L.write!='none')$('lmsg').textContent='Last change: '+L.write;
  var cv=function(x){return x?(x/1000).toFixed(3):'--'},pv=function(x){return x?x.toFixed(1):'--'};
  var LC=['cell_high_l1_mv','cell_high_l2_mv','cell_low_l1_mv','cell_low_l2_mv'],
      LP=['pack_high_l1_v','pack_high_l2_v','pack_low_l1_v','pack_low_l2_v'];
  for(var i=0;i<4;i++){$('lc'+i).textContent=cv(L[LC[i]]);$('lp'+i).textContent=pv(L[LP[i]])}
  var pk=LP.map(function(k){return L[k]}).filter(Boolean);
  $('vlim').textContent=pk.length?'high '+pv(Math.max.apply(null,pk))+' · low '+pv(Math.min.apply(null,pk))+' V':'';
  var PR=L.profiles||{};
  ['normal','storage'].forEach(function(n){
    var p=PR[n];$('pb_'+n).textContent=n[0].toUpperCase()+n.slice(1)+(p?' '+p.high_v.toFixed(1)+' - '+p.low_v.toFixed(1)+' V':'');
  });
  $('prof').textContent=L.profile=='unknown'?'--':L.profile;
  var sl=L.sleep_timeout_s;
  $('lsl').textContent=!sl?'--':sl>=65535?'off (never sleeps)':sl+' s';
  $('gtrip').textContent=s.guard.limit_cell_mv
    ? cv(s.guard.limit_cell_mv)+' V per cell ('+(s.guard.limit_from_bms?'from BMS limit':'configured')+')'
    : '--';
  $('wifi').textContent=s.wifi.ssid+' · '+s.wifi.rssi+' dBm · '+s.ip;
  var m=$('mqtt');m.textContent=s.mqtt.connected?'connected':'disconnected';
  m.className='pill '+(s.mqtt.connected?'on':'off');
  $('mqttsrv').textContent=s.mqtt.server+(s.mqtt.host?' · '+s.mqtt.host:'');
  $('heap').textContent=(s.stats.free_heap/1024).toFixed(1)+' kB';
  $('sweeps').textContent=s.stats.sweeps;

  // The ESP has no clock, so "updated" is the browser's time when this answer
  // arrived; the BMS reading's own age comes from the ESP.
  var age=s.stale_seconds>=65535?'no BMS data':'BMS reading '+s.stale_seconds+' s old';
  $('meta').textContent='updated '+new Date().toLocaleTimeString()+' ('+age+')'
    +' · up '+Math.floor(s.uptime_s/60)+' min · RSSI '+s.rssi
    +' dBm · ok '+s.stats.frames_ok+' · timeouts '+s.stats.timeouts
    +' · crc '+s.stats.checksum_errors;
}

var lastLimits=null;
var LK={cell:['cell_high_l1_mv','cell_high_l2_mv','cell_low_l1_mv','cell_low_l2_mv'],
        pack:['pack_high_l1_v','pack_high_l2_v','pack_low_l1_v','pack_low_l2_v'],
        sleep:['sleep_timeout_s'],nosleep:[],profile:[]};
// Cell limits are shown and typed in volts; the ESP takes whole millivolts.
function isMv(k){return /_mv$/.test(k)}
$('lset').addEventListener('toggle',function(){
  if(!this.open||!lastLimits)return;
  LK.cell.concat(LK.pack,LK.sleep).forEach(function(k){
    var x=lastLimits[k];
    if(x)$('f_'+k).value=isMv(k)?(x/1000).toFixed(3):(k=='sleep_timeout_s'&&x>=65535?'':x);
  });
});
function setProfile(name){saveLimits('profile',name)}
// Click a MOS pill to switch it. Asks for the PIN, then the pill follows what the BMS
// reports on the next readings, not what was clicked.
var mosNow={},mosWant=null;
// Progress is shown in the PIN box itself: it stays open from the PIN to the outcome.
function mosShow(t){$('pinmsg').textContent=t}
function mosEnd(t,ok){
  mosWant=null;mosShow(t);
  if(ok){setTimeout(function(){if(!mosWant)$('pind').close()},1200)}
  else{$('pin').style.display='none';$('pinok').style.display='none';$('pinx').textContent='Close'}
}
function toggleMos(field,id){
  if(!(id in mosNow)||mosWant)return;
  var on=!mosNow[id],label=(field=='charge_mos'?'Charge':'Discharge')+' MOS';
  var d=$('pind'),f=$('pinf');
  $('pinq').textContent='Enter PIN to switch '+label.toLowerCase()+' '+(on?'on':'off');
  $('pin').value='';$('pinmsg').textContent='';
  $('pin').disabled=false;$('pinok').disabled=false;
  $('pin').style.display='';$('pinok').style.display='';$('pinx').textContent='Cancel';
  f.onsubmit=function(e){
    if(!e.submitter||e.submitter.value!='ok')return;
    e.preventDefault();
    var v=$('pin').value;if(!v)return;
    var b=new URLSearchParams();
    b.set(field,on?'on':'off');b.set('pin',v);
    $('pinok').disabled=true;$('pin').disabled=true;
    $('pinmsg').textContent='sending...';
    fetch('/limits',{method:'POST',body:b})
      .then(function(r){return r.text().then(function(t){
        if(r.status==202){
          mosWant={id:id,on:on,t:Date.now(),label:label};
          mosShow('Switching '+label+' '+(on?'on':'off')+'...');
          $('pinx').textContent='Close';
        }else{
          $('pinmsg').textContent=t.trim();
          $('pinok').disabled=false;$('pin').disabled=false;$('pin').value='';$('pin').focus();
        }
      })})
      .catch(function(){$('pinmsg').textContent='request failed';$('pinok').disabled=false;$('pin').disabled=false});
  };
  d.showModal();
  $('pin').focus();
}
function askPin(cb,text){
  var d=$('pind'),f=$('pinf');
  $('pinq').textContent=text||'Enter PIN to change BMS settings';
  $('pin').value='';$('pinmsg').textContent='';
  $('pin').disabled=false;$('pinok').disabled=false;
  $('pin').style.display='';$('pinok').style.display='';$('pinx').textContent='Cancel';
  f.onsubmit=function(e){var v=$('pin').value;
    if(e.submitter&&e.submitter.value=='ok'&&v)setTimeout(function(){cb(v)},0)};
  d.showModal();
  $('pin').focus();
}
function saveLimits(group,profile){
  var b=new URLSearchParams();
  if(group=='profile')b.set('profile',profile);
  else if(group=='nosleep')b.set('sleep_timeout_s','65535');
  else LK[group].forEach(function(k){
    var t=$('f_'+k).value.trim();
    if(t&&isMv(k)){t=t.replace(',','.');t=/^\d+(\.\d+)?$/.test(t)?String(Math.round(parseFloat(t)*1000)):'x'}
    b.set(k,t);
  });
  askPin(function(pin){b.set('pin',pin);postLimits(b)});
}
function postLimits(b){
  $('lmsg').textContent='sending...';
  fetch('/limits',{method:'POST',body:b})
    .then(function(r){return r.text().then(function(t){
      $('lmsg').textContent=t.trim()+(r.status==202?' - the BMS is written between readings; the limits above update when it has read them back':'');
    })})
    .catch(function(){$('lmsg').textContent='request failed'});
}

// One request at a time, the next only after the last has finished. The ESP has
// very little spare memory once TLS is up, and in normal mode its answers take
// seconds: a fixed timer would stack up requests until it runs out and resets.
var every=2000;
function poll(){
  fetch('/status.json',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(s){render(s);every=(s.mode=='high'||mosWant)?(mosWant?1500:2000):10000})
    .catch(function(){
      $('dot').style.background='var(--bad)';
      $('meta').textContent='cannot reach the ESP';
      every=10000;
    })
    .then(function(){setTimeout(poll,every)});
}
poll();
</script></body></html>)HTML";
