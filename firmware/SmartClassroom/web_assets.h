// Local pages. Written into LittleFS (/www) at boot when WEB_ASSETS_VERSION changes.
#pragma once
#define WEB_ASSETS_VERSION "w3"

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Classroom - Local Status</title>
<style>
:root{--bg:#0f172a;--card:#1e293b;--tx:#e2e8f0;--mut:#94a3b8;--ok:#22c55e;--warn:#eab308;--bad:#ef4444;--blue:#3b82f6}
*{box-sizing:border-box}body{margin:0;font-family:system-ui,Segoe UI,Arial,sans-serif;background:var(--bg);color:var(--tx)}
header{padding:12px 16px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px;background:#020617}
h1{font-size:18px;margin:0}.pill{padding:3px 10px;border-radius:99px;font-weight:700;font-size:13px}
.banner{display:none;margin:10px 16px;padding:14px;border-radius:10px;font-size:22px;font-weight:800;text-align:center}
.fire{background:var(--bad);animation:bl .4s infinite alternate}.panic{background:linear-gradient(90deg,#ef4444,#3b82f6)}
.wx{background:#7c3aed}.intr{background:#b91c1c}.cam{background:#0891b2}.cap{background:#ea580c}.cs{background:#f59e0b;color:#000}
@keyframes bl{from{opacity:1}to{opacity:.55}}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:10px;padding:16px}
.c{background:var(--card);border-radius:10px;padding:10px 12px}.k{color:var(--mut);font-size:12px}.v{font-size:20px;font-weight:700;margin-top:4px;word-break:break-all}
.SAFE{color:var(--ok)}.WARNING{color:var(--warn)}.DANGER,.FIRE{color:var(--bad)}footer{color:var(--mut);font-size:12px;padding:0 16px 16px}
</style></head><body>
<header><h1>Smart Classroom <span id="dev"></span> &middot; Room <span id="room">-</span></h1>
<span class="pill" id="conn">-</span></header>
<div class="banner fire" id="bFire">FIRE &ndash; EVACUATE</div>
<div class="banner cs" id="bCs">CANNOT SILENCE &ndash; FLAME PRESENT</div>
<div class="banner panic" id="bPanic">PANIC</div>
<div class="banner wx" id="bWx">WEATHER ALERT</div>
<div class="banner intr" id="bIntr">INTRUDER</div>
<div class="banner cam" id="bCam">CAMERA FAULT</div>
<div class="banner cap" id="bCap">OVER CAPACITY</div>
<div class="grid" id="g"></div>
<footer>Updates every second &middot; <span id="upd">-</span></footer>
<script>
const F=[["time","Local time (Asia/Dhaka)"],["status","Status"],["temp","Temperature"],["rh","Humidity"],["hi","Heat index"],["trend","Heat trend"],
["quality","Data quality"],["flame","Flame"],["door","Door"],["last_uid","Last card UID"],["check","Last check-in"],["limits","Warning / Danger limits"],
["weather_line","Weather"],["cam_position","Camera position"],["people","People count"],["presence","Presence (Wi-Fi)"],["zone","Predicted zone"],
["hand","Last hand raised"],["pending","Records waiting"],["rssi","Wi-Fi signal"],["chip","Chip temperature"],["fw","Firmware"],["uptime","Uptime"]];
const g=document.getElementById('g');F.forEach(([k,l])=>{g.insertAdjacentHTML('beforeend',`<div class="c"><div class="k">${l}</div><div class="v" id="f_${k}">-</div></div>`)});
const $=i=>document.getElementById(i),set=(k,v,c)=>{const e=$('f_'+k);e.textContent=v;e.className='v '+(c||'')};
const show=(i,on,t)=>{const e=$(i);e.style.display=on?'block':'none';if(t!==undefined)e.textContent=t};
const ft=t=>t?new Date(t*1000).toLocaleTimeString('en-GB',{timeZone:'Asia/Dhaka'}):'-';
async function tick(){try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();
$('dev').textContent=d.device_id;$('room').textContent=d.room_id||'-';
const cc={LIVE:'#22c55e',SYNCING:'#eab308',OFFLINE:'#3b82f6',SETUP:'#3b82f6',CONNECTING:'#3b82f6'};$('conn').textContent=d.conn;$('conn').style.background=cc[d.conn]||'#64748b';
set('time',d.time);set('status',d.fire?'FIRE':d.status,d.fire?'FIRE':d.status);
set('temp',d.temp_c!=null?d.temp_c.toFixed(1)+' °C':'-');set('rh',d.rh!=null?Math.round(d.rh)+' %':'-');set('hi',d.heat_index_c!=null?d.heat_index_c.toFixed(1)+' °C':'-');
set('trend',d.trend);set('quality',d.quality+(d.dht_ok?'':' (DHT11 error)')+(d.rfid_ok?'':' (RFID error)'),d.quality=='good'?'SAFE':'DANGER');
set('flame',d.flame?'YES':'no',d.flame?'FIRE':'');set('door',d.door,d.door=='UNLOCKED'?'WARNING':'SAFE');set('last_uid',d.last_uid);
set('check',d.check_result=='-'?'-':`${d.check_name} · ${d.check_result} · ${d.check_reason} · ${ft(d.check_ts)}`,d.check_result=='GRANTED'?'SAFE':d.check_result=='DENIED'?'DANGER':'');
set('limits',`${d.warn_c.toFixed(1)} / ${d.danger_c.toFixed(1)} °C`);set('weather_line',d.weather_line||'-');
set('cam_position',d.cam_position+(d.night?' · NIGHT':''));set('people',d.people<0?'-':d.people);set('presence',d.presence);set('zone',d.zone);
set('hand',d.hand_seat=='-'?'-':`Seat ${d.hand_seat} · ${ft(d.hand_ts)}`);set('pending',d.pending);set('rssi',d.rssi?d.rssi+' dBm':'-');
set('chip',d.chip_temp_c.toFixed(1)+' °C');set('fw',d.fw);set('uptime',d.uptime_s+' s');
show('bFire',d.fire,d.fire_silenced?'FIRE – EVACUATE (buzzer silenced, door stays UNLOCKED)':'FIRE – EVACUATE');show('bCs',d.cannot_silence);show('bPanic',d.panic);
show('bWx',d.weather_alert,'WEATHER ALERT: '+d.weather_text);show('bIntr',d.intruder);show('bCam',d.cam_fault!='OK','CAMERA FAULT: '+d.cam_fault);show('bCap',d.over_capacity);
$('upd').textContent='last update '+new Date().toLocaleTimeString();}catch(e){$('upd').textContent='device not reachable'}}
tick();setInterval(tick,1000);
</script></body></html>)HTML";

static const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Classroom - Setup</title>
<style>body{font-family:system-ui,Arial;margin:0;background:#0f172a;color:#e2e8f0}main{max-width:460px;margin:auto;padding:16px}
h1{font-size:20px}label{display:block;margin-top:12px;font-size:14px;color:#94a3b8}input,select{width:100%;padding:10px;margin-top:4px;border-radius:8px;border:1px solid #334155;background:#1e293b;color:#e2e8f0;font-size:16px;box-sizing:border-box}
button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#3b82f6;color:#fff;font-size:16px;font-weight:700}
#msg{margin-top:14px;padding:12px;border-radius:8px;display:none}.err{background:#7f1d1d}.ok{background:#14532d}.info{background:#1e3a8a}
li{list-style:none;padding:8px;border-bottom:1px solid #334155;cursor:pointer}ul{padding:0;max-height:220px;overflow:auto;background:#1e293b;border-radius:8px}</style></head>
<body><main><h1>Smart Classroom setup &middot; <span id="dev"></span></h1>
<div>Nearby networks (tap to choose):</div><ul id="nets"><li>Scanning...</li></ul>
<form id="f"><label>Wi-Fi network (SSID)<input name="ssid" id="ssid" required></label>
<label>Wi-Fi password<input name="pass" type="password"></label>
<label>Room ID<input name="room" required></label>
<label>Device token<input name="token" required autocomplete="off"></label>
<label>Platform address (HTTPS)<input name="server" id="server"></label>
<button>Save and connect</button></form><div id="msg"></div></main>
<script>
const $=i=>document.getElementById(i);
const bars=r=>r>-55?'▂▄▆█':r>-67?'▂▄▆':r>-78?'▂▄':'▂';
async function scan(){try{const d=await (await fetch('/scan')).json();if(d.list.length){$('nets').innerHTML=d.list.sort((a,b)=>b.rssi-a.rssi).map(n=>`<li data-s="${n.ssid}">${n.ssid} &nbsp; ${bars(n.rssi)} ${n.rssi} dBm ${n.secure?'🔒':''}</li>`).join('')}}catch(e){}}
$('nets').onclick=e=>{if(e.target.dataset.s)$('ssid').value=e.target.dataset.s};
fetch('/setupstatus').then(r=>r.json()).then(d=>{$('dev').textContent=d.device_id;$('server').value=d.server;show(d)});
function show(d){const m=$('msg');if(d.state=='idle'){m.style.display='none';return}m.style.display='block';m.className=d.state=='error'?'err':d.state=='ok'?'ok':'info';m.textContent=(d.state=='error'?'Error: ':'')+d.msg}
$('f').onsubmit=async e=>{e.preventDefault();const r=await fetch('/save',{method:'POST',body:new URLSearchParams(new FormData($('f')))});
if(!r.ok){show({state:'error',msg:(await r.json()).error});return}show({state:'connecting',msg:'Connecting...'});poll()};
async function poll(){for(let i=0;i<40;i++){await new Promise(r=>setTimeout(r,1000));try{const d=await (await fetch('/setupstatus')).json();show(d);if(d.state!='connecting')return}catch(e){}}}
scan();setInterval(scan,5000);
</script></body></html>)HTML";
