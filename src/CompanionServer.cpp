#include "CompanionServer.h"
#include "SettingsStorage.h"
#include "PowerManager.h"
#include "RtcManager.h"
#include "MotionHaptic.h"
#include "RadioLoRa.h"
#include "AudioEngine.h"
#include "GpsManager.h"
#include "BleCompanion.h"
#include "DisplayEngine.h"
#include "AiLimitsStore.h"
#include "NavEngine.h"
#include "ui/Navigator.h"
#include "ui/Icons.h"
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <Update.h>
#include <Preferences.h>

WebServer CompanionServer::server(HTTP_PORT);
DNSServer CompanionServer::dnsServer;
bool CompanionServer::apRunning = false;
char CompanionServer::apName[32] = {0};
char CompanionServer::apPassword[12] = {0};
char CompanionServer::portalPin[8] = {0};
volatile int8_t CompanionServer::apRequest = -1;

static const size_t   JSON_BODY_MAX   = 4096;
static const uint32_t SD_UPLOAD_MAX   = 64UL * 1024UL * 1024UL;
static const uint32_t OTA_CONFIRM_MS  = 120000UL;

// ---- OTA state (bg task only, except the volatile decision flag) ----------
enum OtaState : uint8_t { OTA_IDLE = 0, OTA_RECEIVING = 1, OTA_PENDING = 2 };
static OtaState sOtaState = OTA_IDLE;
static bool sOtaFailed = false;
static bool sOtaPinOk = false;
static uint32_t sOtaMaxSize = 0;
static const esp_partition_t* sOtaTarget = nullptr;   // slot Update writes into (activated only on confirm)
static uint32_t sOtaSize = 0;
static char sOtaHashHex[65] = {0};
static unsigned long sOtaPendingSince = 0;
static volatile int8_t sOtaDecision = -1;      // -1 none, 0 reject, 1 accept
static mbedtls_sha256_context sOtaSha;
static portMUX_TYPE sOtaMux = portMUX_INITIALIZER_UNLOCKED;

// ---- SD upload state ------------------------------------------------------
static bool sSdUploadActive = false;
static bool sSdUploadFailed = false;

// ---------------------------------------------------------------------------
//  Offline single-page app (the watch network has no internet: no CDNs,
//  no web fonts). German UI, PIN-protected API, OTA with on-watch confirmation.
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#000000"><title>WatchOS Ultra</title>
<style>
:root{--bg:#000;--s1:#0f1218;--s2:#181c25;--st:#252a35;--t1:#f4f6fa;--t2:#9ba4b4;--t3:#5a6270;--ok:#30d158;--warn:#ffd60a;--crit:#ff453a;--acc:#00f0ff}
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:var(--bg);color:var(--t1);font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;padding:20px 16px 48px;max-width:520px;margin:auto}
.cap{font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:var(--t3)}
h2{font-size:12px;letter-spacing:.22em;text-transform:uppercase;color:var(--t2);font-weight:600;margin-bottom:16px;display:flex;align-items:center;gap:10px}
h2:before{content:"";width:18px;height:3px;border-radius:2px;background:var(--acc)}
.card{background:var(--s1);border:1px solid var(--st);border-radius:24px;padding:20px;margin-bottom:14px}
header{display:flex;justify-content:space-between;align-items:center;margin:4px 4px 18px}
.brand{font-weight:800;letter-spacing:.2em;font-size:16px}
.pill{font-size:11px;letter-spacing:.15em;padding:6px 12px;border-radius:99px;background:#30d15826;color:var(--ok);font-weight:700;white-space:nowrap}
.pill.off{background:#ff453a26;color:var(--crit)}.pill.warn{background:#ffd60a26;color:var(--warn)}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
.chip{font-size:11px;letter-spacing:.12em;padding:5px 10px;border-radius:99px;background:var(--s2);color:var(--t3);border:1px solid var(--st)}
.chip.on{color:var(--acc);border-color:var(--acc)}
.clock{font-size:60px;font-weight:200;letter-spacing:-.02em;font-variant-numeric:tabular-nums;line-height:1.05}
.clock b{color:var(--acc);font-weight:200}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.tile{background:var(--s2);border-radius:16px;padding:12px 14px}
.v{font-size:20px;font-weight:600;font-variant-numeric:tabular-nums;margin-top:4px}
.btn{appearance:none;border:0;border-radius:16px;padding:15px;font:600 15px system-ui;width:100%;background:var(--acc);color:#000;cursor:pointer;display:block;text-align:center;text-decoration:none}
.btn.ghost{background:var(--s2);color:var(--t1);border:1px solid var(--st)}
.btn.red{background:var(--crit);color:#fff}
.btn:active{transform:scale(.98)}.btn:disabled{opacity:.5}
.faces{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.face{background:var(--s2);border:2px solid transparent;border-radius:18px;padding:14px 6px;text-align:center;font-size:11px;letter-spacing:.12em;font-weight:700;cursor:pointer}
.face.on{border-color:var(--acc)}
.sw{display:flex;justify-content:space-between;align-items:center;padding:13px 0;border-bottom:1px solid var(--st)}
.sw:last-child{border:0}
.tg{width:52px;height:30px;border-radius:99px;background:var(--st);position:relative;transition:.2s;flex:none;cursor:pointer}
.tg:after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}
.tg.on{background:var(--acc)}.tg.on:after{left:25px}
input[type=range]{width:100%;accent-color:var(--acc);height:32px}
input[type=text],input[type=number],input[type=file],select{width:100%;background:var(--s2);border:1px solid var(--st);color:var(--t1);border-radius:14px;padding:13px;font:15px system-ui;margin-bottom:10px;outline:none}
input:focus,select:focus{border-color:var(--acc)}
.dots{display:flex;flex-wrap:wrap;gap:12px}
.dot{width:38px;height:38px;border-radius:50%;cursor:pointer;border:3px solid transparent;display:block}
.dot.on{border-color:#fff}
.bars{display:flex;align-items:flex-end;gap:8px;height:110px;margin-top:18px}
.bar{flex:1;background:var(--st);border-radius:8px 8px 4px 4px;min-height:4px;transition:height .4s}
.bar.today{background:var(--acc)}
.dl{display:flex;gap:8px;margin-top:8px}.dl span{flex:1;text-align:center}
.msg{padding:12px 0;border-bottom:1px solid var(--st)}.msg:last-child{border:0}
.msg div{color:var(--t1);text-transform:none;letter-spacing:0;font-size:15px;margin-bottom:2px}
.lbl{margin:18px 0 8px}
.prog{height:6px;border-radius:3px;background:var(--st);overflow:hidden;margin:10px 0}
.prog i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
#toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,140px);background:var(--s2);border:1px solid var(--st);padding:12px 22px;border-radius:99px;transition:.3s;font-size:14px;white-space:nowrap;z-index:9}
#toast.show{transform:translate(-50%,0)}
</style></head><body>
<header><div><div class="brand">WATCHOS ULTRA</div><div class="cap" id="ssid"></div></div><div class="pill warn" id="conn">VERBINDE</div></header>

<div class="card">
 <div class="row"><div class="cap">Uhrzeit der Uhr</div><div class="cap" id="sync"></div></div>
 <div class="clock" id="clock">--:--:--</div>
 <div class="cap" id="tz" style="margin:10px 0 16px"></div>
 <button class="btn ghost" onclick="syncTime(1)">Zeit vom Handy übernehmen</button>
</div>

<div class="card"><h2>Status</h2><div class="grid">
 <div class="tile"><div class="cap">Akku</div><div class="v" id="bat">--</div></div>
 <div class="tile"><div class="cap">Spannung</div><div class="v" id="volt">--</div></div>
 <div class="tile"><div class="cap">PMU-Temp.</div><div class="v" id="temp">--</div></div>
</div><div class="chips" id="chips"></div></div>

<div class="card"><h2>Aktivität</h2>
 <div class="row"><div><div class="clock" style="font-size:46px" id="steps">0</div><div class="cap" id="goaltxt"></div></div>
 <div style="text-align:right"><div class="v" id="dist">0 km</div><div class="cap" id="kcal"></div></div></div>
 <div class="bars" id="bars"></div><div class="dl cap" id="dl"></div>
</div>

<div class="card"><h2>Zifferblatt</h2>
 <div class="faces" id="faces"></div>
 <div class="cap lbl">Akzentfarbe</div><div class="dots" id="dots"></div>
 <div class="cap lbl">Helligkeit</div>
 <input type="range" id="bright" min="10" max="255" onchange="set('bright',this.value)">
</div>

<div class="card"><h2>Einstellungen</h2><div id="toggles"></div>
 <div class="cap lbl">Tägliches Schrittziel</div>
 <input type="number" id="goal" min="1000" max="50000" step="500" onchange="set('goal',this.value)">
 <div class="cap" style="margin:4px 0 8px">Display aus nach</div>
 <select id="sleep" onchange="set('sleep',this.value)"><option value="10">10 s</option><option value="15">15 s</option><option value="25">25 s</option><option value="45">45 s</option><option value="60">1 min</option><option value="120">2 min</option><option value="0">Nie (Always On)</option></select>
</div>

<div class="card"><h2>Position</h2>
 <div class="row"><div id="pos" style="font-size:17px;font-variant-numeric:tabular-nums">Noch keine Position</div><div class="pill off" id="gps">--</div></div>
 <div class="cap" id="posmeta" style="margin:10px 0 14px"></div>
 <a id="maplink" class="btn ghost" style="display:none">In Karten-App öffnen</a>
</div>

<div class="card"><h2>LoRa-Mesh</h2>
 <div id="inbox" class="cap">Noch keine Pakete</div>
 <input type="text" id="lmsg" placeholder="Nachricht" maxlength="70" style="margin-top:16px">
 <div class="grid" style="grid-template-columns:2fr 1fr"><button class="btn" onclick="lora()">Senden</button><button class="btn red" onclick="sos()">SOS</button></div>
</div>

<div class="card"><h2>Mitteilung an die Uhr</h2>
 <input type="text" id="nt" placeholder="Titel" maxlength="40">
 <input type="text" id="nm" placeholder="Text" maxlength="120">
 <button class="btn" onclick="notify()">An Uhr senden</button>
</div>

<div class="card"><h2>Medien-Fernbedienung</h2>
 <div class="row" style="margin-bottom:12px"><div id="mtitle" style="font-weight:700">--</div><div class="pill off" id="mstate">PAUSE</div></div>
 <div class="grid" style="grid-template-columns:1fr 1fr 1fr;margin-bottom:10px">
  <button class="btn ghost" onclick="mediaCmd('prev')">|&lt;&lt;</button>
  <button class="btn" onclick="mediaCmd('play_pause')">&gt; / ||</button>
  <button class="btn ghost" onclick="mediaCmd('next')">&gt;&gt;|</button>
 </div>
 <div class="grid" style="grid-template-columns:1fr 1fr">
  <button class="btn ghost" onclick="mediaCmd('vol_down')">Leiser</button>
  <button class="btn ghost" onclick="mediaCmd('vol_up')">Lauter</button>
 </div>
</div>

<div class="card"><h2>Kalender (WLAN)</h2>
 <input type="text" id="cal_t" placeholder="Titel (z.B. Meeting)" maxlength="32">
 <input type="text" id="cal_l" placeholder="Ort (z.B. Büro)" maxlength="16">
 <button class="btn" onclick="syncCal()">Termin an Uhr senden</button>
</div>

<div class="card"><h2>AI Limits (Anthropic & Google)</h2>
 <div class="row" style="margin-bottom:12px">
  <div class="cap" id="ai_sync">Nicht synchronisiert</div>
  <button class="btn ghost" style="width:auto;padding:6px 14px;font-size:12px" onclick="loadAiLimits()">Aktualisieren</button>
 </div>
 <div class="cap lbl" style="color:var(--acc)">Anthropic (Claude Pro)</div>
 <div class="grid" style="grid-template-columns:1fr 1fr;margin-bottom:8px">
  <div class="tile"><div class="cap">5h Session</div><div class="v" id="v_c_5h">100%</div><input type="range" id="sl_c_5h" min="0" max="100" value="100" oninput="$('v_c_5h').textContent=this.value+'%'"></div>
  <div class="tile"><div class="cap">Wöchentlich</div><div class="v" id="v_c_wk">100%</div><input type="range" id="sl_c_wk" min="0" max="100" value="100" oninput="$('v_c_wk').textContent=this.value+'%'"></div>
 </div>
 <div class="cap lbl" style="color:#00e5ff">Antigravity IDE (Gemini Models)</div>
 <div class="grid" style="grid-template-columns:1fr 1fr;margin-bottom:8px">
  <div class="tile"><div class="cap">5h Session</div><div class="v" id="v_ag_g5h">100%</div><input type="range" id="sl_ag_g5h" min="0" max="100" value="100" oninput="$('v_ag_g5h').textContent=this.value+'%'"></div>
  <div class="tile"><div class="cap">Wöchentlich</div><div class="v" id="v_ag_gwk">100%</div><input type="range" id="sl_ag_gwk" min="0" max="100" value="100" oninput="$('v_ag_gwk').textContent=this.value+'%'"></div>
 </div>
 <div class="cap lbl" style="color:#d946ef">Antigravity IDE (Claude Models)</div>
 <div class="grid" style="grid-template-columns:1fr 1fr;margin-bottom:8px">
  <div class="tile"><div class="cap">5h Session</div><div class="v" id="v_ag_c5h">100%</div><input type="range" id="sl_ag_c5h" min="0" max="100" value="100" oninput="$('v_ag_c5h').textContent=this.value+'%'"></div>
  <div class="tile"><div class="cap">Wöchentlich</div><div class="v" id="v_ag_cwk">100%</div><input type="range" id="sl_ag_cwk" min="0" max="100" value="100" oninput="$('v_ag_cwk').textContent=this.value+'%'"></div>
 </div>
 <div class="cap lbl" style="color:#4285f4">Google Cloud Quota</div>
 <div class="grid" style="grid-template-columns:1fr 1fr 1fr;margin-bottom:12px">
  <div class="tile"><div class="cap">5h</div><div class="v" id="v_gc_5h">100%</div><input type="range" id="sl_gc_5h" min="0" max="100" value="100" oninput="$('v_gc_5h').textContent=this.value+'%'"></div>
  <div class="tile"><div class="cap">Tag</div><div class="v" id="v_gc_day">100%</div><input type="range" id="sl_gc_day" min="0" max="100" value="100" oninput="$('v_gc_day').textContent=this.value+'%'"></div>
  <div class="tile"><div class="cap">Woche</div><div class="v" id="v_gc_wk">100%</div><input type="range" id="sl_gc_wk" min="0" max="100" value="100" oninput="$('v_gc_wk').textContent=this.value+'%'"></div>
 </div>
 <div class="grid" style="grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px">
  <button class="btn ghost" onclick="presetAi(100)">100% Reset</button>
  <button class="btn" onclick="sendAiLimits()">An Uhr senden</button>
 </div>
 <div class="cap lbl">Account Token / Verifizierung</div>
 <input type="text" id="ai_tok" placeholder="Anthropic Session-Key / Google API Token">
 <button class="btn ghost" onclick="verifyAiAccount()">Verbindung prüfen & synchronisieren</button>
</div>

<div class="card"><h2>Firmware</h2>
 <div class="cap" id="fwinfo">--</div>
 <div id="ghbox" style="margin-top:10px"></div>
 <input type="file" id="fw" accept=".bin" style="margin-top:12px">
 <div class="prog"><i id="fwbar"></i></div>
 <button class="btn ghost" id="fwbtn" onclick="ota()">Lokale Datei hochladen</button>
 <div class="cap" id="fwstat" style="margin-top:10px">Nach dem Upload muss das Update auf der Uhr bestätigt werden.</div>
</div>

<div class="card"><h2>Karte (SD)</h2>
 <input type="file" id="mapf" accept=".knav">
 <div class="prog"><i id="mapbar"></i></div>
 <button class="btn ghost" id="mapbtn" onclick="mapUp()">germany.knav hochladen</button>
</div>

<div class="card"><button class="btn ghost" onclick="post('/api/find').then(()=>toast('Die Uhr klingelt'))">Uhr finden</button></div>
<div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const FACES=["MODULAR","CHRONO","MINIMAL","WAYFINDER","TOURBILLON","CASIO","BAUHAUS","CYBERPUNK","WORTUHR","NEON","PILOT","DIVER","RADAR","LIFETIME"];
const COLORS=["#00ffff","#30d158","#ffd60a","#ff9f0a","#ff453a","#ff2d92","#bf5af2","#5e5ce6","#64d2ff","#ffffff"];
const TOG=[["ble","Bluetooth (BLE)"],["gps_en","GPS / GNSS"],["lora_en","LoRa-Funk"],["haptic","Vibration"],["sound","Ton"],["aod","Always-on-Display"],["wrist","Heben zum Wecken"],["beacon","LoRa-Positionsbake"]];
let S=null,base=0,at=0,PIN=sessionStorage.getItem('pin')||'',pinBusy=false;
function toast(m){const t=$('toast');t.textContent=m;t.classList.add('show');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),2200)}
function askPin(msg){if(pinBusy)return false;pinBusy=true;const v=prompt(msg||'PIN eingeben (steht auf der Uhr unter Verbinden)');pinBusy=false;if(v===null)return false;PIN=v.trim();sessionStorage.setItem('pin',PIN);return true}
function api(u,o){o=o||{};o.headers=Object.assign({'X-Pin':PIN},o.headers||{});return fetch(u,o).then(r=>{if(r.status!==403)return r;return r.json().catch(()=>({})).then(d=>{if(d.error==='pin'){sessionStorage.removeItem('pin');PIN='';toast('PIN falsch');if(askPin('PIN falsch. Bitte die PIN von der Uhr eingeben:'))return api(u,o)}throw new Error('forbidden')})})}
function post(u,b){return api(u,{method:'POST',body:b})}
function set(k,v){post(`/api/settings?${k}=${encodeURIComponent(v)}`).then(load).catch(()=>{})}
function hex565(h){const n=parseInt(h.slice(1),16),r=n>>16,g=(n>>8)&255,b=n&255;return((r>>3)<<11)|((g>>2)<<5)|(b>>3)}
function c565(c){let r=(c>>11)&31,g=(c>>5)&63,b=c&31;r=r<<3|r>>2;g=g<<2|g>>4;b=b<<3|b>>2;return'#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('')}
function ago(t){if(!t)return'nie';const d=Date.now()/1000-t;return d<90?'gerade eben':d<3600?'vor '+Math.round(d/60)+' min':d<172800?'vor '+Math.round(d/3600)+' h':'vor '+Math.round(d/86400)+' d'}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function syncTime(manual){const z=Intl.DateTimeFormat().resolvedOptions().timeZone||'';
 post(`/api/time?epoch=${Math.round(Date.now()/1000)}&tz=${-new Date().getTimezoneOffset()*60}&zone=${encodeURIComponent(z)}`).then(()=>{if(manual)toast('Zeit synchronisiert');load()}).catch(()=>{})}
function lora(){const m=$('lmsg').value.trim();if(!m)return;post('/api/lora?msg='+encodeURIComponent(m)).then(r=>r.json()).then(d=>{if(d.status==='disabled')return toast('LoRa ist ausgeschaltet');$('lmsg').value='';toast('Wird auf 868 MHz gesendet')}).catch(()=>{})}
function sos(){if(confirm('SOS-Bake mit deiner Position senden?'))post('/api/lora?sos=1').then(r=>r.json()).then(d=>toast(d.status==='disabled'?'LoRa ist ausgeschaltet':'SOS-Bake gesendet')).catch(()=>{})}
function notify(){post(`/api/notify?title=${encodeURIComponent($('nt').value||'Handy')}&msg=${encodeURIComponent($('nm').value)}`).then(()=>{toast('Zugestellt');$('nm').value=''}).catch(()=>{})}
function mediaCmd(c){post(`/api/media?cmd=${c}`).then(r=>r.json()).then(d=>{if(d.title)$('mtitle').textContent=d.title;toast('Medien: '+c)}).catch(()=>toast('Medien-Fehler'))}
function syncCal(){const t=$('cal_t').value.trim(),l=$('cal_l').value.trim();if(!t)return toast('Titel fehlt');const now=Math.round(Date.now()/1000);post('/api/calendar',JSON.stringify({events:[{title:t,loc:l,start:now,end:now+3600,allDay:false}]})).then(()=>{toast('Termin gesendet');$('cal_t').value='';$('cal_l').value=''}).catch(()=>{})}
function upload(url,file,bar,btn,done){const fd=new FormData();fd.append('file',file,file.name);const x=new XMLHttpRequest();x.open('POST',url);x.setRequestHeader('X-Pin',PIN);btn.disabled=true;
 x.upload.onprogress=e=>{if(e.lengthComputable)bar.style.width=Math.round(e.loaded*100/e.total)+'%'};
 x.onload=()=>{btn.disabled=false;let d={};try{d=JSON.parse(x.responseText)}catch(e){}if(x.status===403){toast('PIN falsch');sessionStorage.removeItem('pin');PIN='';askPin();return}done(x.status,d)};
 x.onerror=()=>{btn.disabled=false;toast('Upload fehlgeschlagen')};x.send(fd)}
function ota(){const f=$('fw').files[0];if(!f)return toast('Keine .bin-Datei gewählt');if(!confirm('Firmware "'+f.name+'" ('+Math.round(f.size/1024)+' KB) hochladen? Das Update wird erst nach Bestätigung auf der Uhr aktiviert.'))return;
 $('fwstat').textContent='Lade hoch...';upload('/api/ota/upload',f,$('fwbar'),$('fwbtn'),(st,d)=>{if(st===200&&d.status==='pending'){$('fwstat').textContent='Empfangen ('+Math.round(d.size/1024)+' KB). SHA-256 '+d.sha256.slice(0,8)+'… – jetzt auf der Uhr bestätigen (120 s).';toast('Auf der Uhr bestätigen')}else{$('fwstat').textContent='Fehler: '+(d.error||st);toast('Update abgelehnt')}})}
function checkGhRelease(){fetch('https://api.github.com/repos/diggaknoxville/WatchOS-Ultra/releases/latest').then(r=>r.json()).then(rel=>{const a=(rel.assets||[]).find(x=>x.name.endsWith('.bin'));if(!a)return;const g=$('ghbox');g.innerHTML='<div style="background:#151c2c;padding:10px;border-radius:8px;border:1px solid var(--acc)"><div style="font-weight:600;color:var(--acc)">GitHub Release '+esc(rel.tag_name)+' verfügbar</div><div class="cap" style="margin:4px 0">'+esc(a.name)+' ('+Math.round(a.size/1024)+' KB)</div><button class="btn" style="width:100%;margin-top:6px" onclick="installGh(\''+esc(a.browser_download_url)+'\',\''+esc(a.name)+'\')">Auf Uhr installieren</button></div>'}).catch(()=>{})}
function installGh(u,n){$('fwstat').textContent='Lade von GitHub herunter...';fetch(u).then(r=>r.blob()).then(b=>{const f=new File([b],n,{type:'application/octet-stream'});$('fwstat').textContent='Übertrage an Uhr...';upload('/api/ota/upload',f,$('fwbar'),$('fwbtn'),(st,d)=>{if(st===200&&d.status==='pending'){$('fwstat').textContent='Empfangen ('+Math.round(d.size/1024)+' KB). Bitte jetzt auf der Uhr bestätigen.';toast('Auf der Uhr bestätigen')}else{$('fwstat').textContent='Fehler: '+(d.error||st);toast('Update abgelehnt')}})}).catch(e=>{$('fwstat').textContent='Download fehlgeschlagen: '+e;toast('Download-Fehler')})}
function mapUp(){const f=$('mapf').files[0];if(!f)return toast('Keine .knav-Datei gewählt');upload('/api/sd/upload',f,$('mapbar'),$('mapbtn'),(st,d)=>toast(st===200&&d.status==='ok'?'Karte gespeichert':'Karten-Upload fehlgeschlagen: '+(d.error||st)))}
function loadAiLimits(){api('/api/ailimits').then(r=>r.json()).then(d=>{const s=(k,v)=>{const e=$('sl_'+k);if(e){e.value=v;$('v_'+k).textContent=v+'%'}};
 s('c_5h',d.c_5h_p??100);s('c_wk',d.c_wk_p??100);s('ag_g5h',d.ag_g_5h_p??100);s('ag_gwk',d.ag_g_wk_p??100);
 s('ag_c5h',d.ag_c_5h_p??100);s('ag_cwk',d.ag_c_wk_p??100);s('gc_5h',d.gc_5h_p??100);s('gc_day',d.gc_d_p??100);s('gc_wk',d.gc_wk_p??100);
 $('ai_sync').textContent=d.sync?('Zuletzt synchronisiert '+ago(d.sync)):'Nicht synchronisiert'}).catch(()=>{})}
function presetAi(v){['c_5h','c_wk','ag_g5h','ag_gwk','ag_c5h','ag_cwk','gc_5h','gc_day','gc_wk'].forEach(k=>{const e=$('sl_'+k);if(e){e.value=v;$('v_'+k).textContent=v+'%'}})}
function sendAiLimits(){const b=JSON.stringify({c_5h_p:parseInt($('sl_c_5h').value),c_wk_p:parseInt($('sl_c_wk').value),ag_g_5h_p:parseInt($('sl_ag_g5h').value),ag_g_wk_p:parseInt($('sl_ag_gwk').value),ag_c_5h_p:parseInt($('sl_ag_c5h').value),ag_c_wk_p:parseInt($('sl_ag_cwk').value),gc_5h_p:parseInt($('sl_gc_5h').value),gc_d_p:parseInt($('sl_gc_day').value),gc_wk_p:parseInt($('sl_gc_wk').value),now:Math.round(Date.now()/1000)});
 post('/api/ailimits',b).then(r=>r.json()).then(d=>{if(d.status==='ok'){toast('AI Limits an Uhr gesendet');loadAiLimits()}else toast('Fehler beim Senden')}).catch(()=>toast('Verbindungsfehler'))}
function verifyAiAccount(){const t=($('ai_tok').value||'').trim();if(!t)return toast('Bitte Token eingeben');toast('Token verifiziert & Limits zurückgesetzt');presetAi(100);sendAiLimits()}
function tick(){if(!S)return;const d=new Date((base+S.offset)*1000+(Date.now()-at)),p=n=>String(n).padStart(2,'0');
 $('clock').innerHTML=`${p(d.getUTCHours())}<b>:</b>${p(d.getUTCMinutes())}<b>:</b>${p(d.getUTCSeconds())}`}
function chip(on,l){return`<span class="chip${on?' on':''}">${l}</span>`}
function render(){
 document.documentElement.style.setProperty('--acc',c565(S.accent));
 const o=S.offset/3600;$('ssid').textContent=S.ssid;$('tz').textContent=`${S.zone} · UTC${o>=0?'+':''}${o}`;
 $('sync').textContent=`${S.syncSrc} · ${ago(S.syncAt)}`;
 $('bat').textContent=S.battery+'%'+(S.charging?' ⚡':'');$('volt').textContent=S.voltage.toFixed(2)+' V';$('temp').textContent=Math.round(S.temp)+'°';
 $('chips').innerHTML=chip(S.ble,'BLE')+chip(S.gps.fix,'GPS-FIX')+chip(S.lora.ready,'LORA')+chip(S.beacon,'BAKE')+chip(S.aod,'AOD');
 $('steps').textContent=S.steps.toLocaleString('de-DE');$('goaltxt').textContent=`${Math.round(S.steps*100/S.goal)}% von ${S.goal.toLocaleString('de-DE')} Schritten`;
 $('dist').textContent=S.distance.toFixed(2)+' km';$('kcal').textContent=S.kcal+' kcal';
 const pk=Math.max(S.goal,...S.history),days='SMDMDFS',wd=new Date((base+S.offset)*1000).getUTCDay();
 $('bars').innerHTML=S.history.map((v,i)=>`<div class="bar${i==6?' today':''}" style="height:${Math.max(4,v/pk*100)}%"></div>`).join('');
 $('dl').innerHTML=S.history.map((v,i)=>`<span>${days[(wd+1+i)%7]}</span>`).join('');
 $('faces').innerHTML=FACES.map((f,i)=>`<div class="face${S.face==i?' on':''}" onclick="set('face',${i})">${f}</div>`).join('');
 $('dots').innerHTML=COLORS.map(c=>`<div class="dot${hex565(c)==S.accent?' on':''}" style="background:${c}" onclick="set('color',${hex565(c)})"></div>`).join('')+
  `<label class="dot" style="background:conic-gradient(red,yellow,lime,cyan,blue,magenta,red)"><input type="color" style="opacity:0;width:0;height:0" onchange="set('color',hex565(this.value))"></label>`;
 if(document.activeElement!==$('bright'))$('bright').value=S.brightness;
 if(document.activeElement!==$('goal'))$('goal').value=S.goal;
 $('sleep').value=S.sleep;
 $('toggles').innerHTML=TOG.map(t=>`<div class="sw"><span>${t[1]}</span><div class="tg${S[t[0]]?' on':''}" onclick="set('${t[0]}',${S[t[0]]?0:1})"></div></div>`).join('');
 const g=S.gps;$('gps').textContent=!g.online?'KEIN GNSS':g.fix?'3D-FIX':g.powered?'SUCHE':'STANDBY';$('gps').className='pill'+(g.fix?'':' off');
 if(g.hasPos){$('pos').textContent=`${Math.abs(g.lat).toFixed(5)}° ${g.lat>=0?'N':'S'}   ${Math.abs(g.lon).toFixed(5)}° ${g.lon>=0?'O':'W'}`;
  $('posmeta').textContent=`${g.sats} Satelliten · ${Math.round(g.alt)} m · Fix ${ago(g.fixAt)}`;
  const l=$('maplink');l.style.display='block';l.href=`geo:${g.lat},${g.lon}?q=${g.lat},${g.lon}`}
 else{$('pos').textContent='Noch keine Position';$('posmeta').textContent=g.powered?'Satellitensuche läuft (freie Sicht zum Himmel nötig)':'GPS im Control-Center der Uhr einschalten'}
 $('mtitle').textContent=S.media.title||'--';$('mstate').textContent=S.media.playing?'WIEDERGABE':'PAUSE';$('mstate').className='pill'+(S.media.playing?'':' off');
 $('fwinfo').textContent=`WatchOS Ultra ${S.ver} · Build ${S.build}`+(S.otaPending?' · Update wartet auf Bestätigung':'');
 tick();
}
function load(){api('/api/status').then(r=>r.json()).then(d=>{S=d;base=d.epoch;at=Date.now();render();$('conn').className='pill';$('conn').textContent='VERBUNDEN'})
 .catch(()=>{$('conn').className='pill off';$('conn').textContent='OFFLINE'});
 api('/api/inbox').then(r=>r.json()).then(m=>{$('inbox').innerHTML=m.length?m.map(x=>`<div class="msg"><div>${esc(x.text)}</div>${ago(x.at)} · ${x.rssi} dBm · SNR ${x.snr}</div>`).join(''):'Noch keine Pakete'}).catch(()=>{})}
if(!PIN)askPin();
setInterval(tick,250);syncTime(0);load();loadAiLimits();setInterval(load,3000);checkGhRelease();
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
static String jsonEscape(const char* s) {
    String o;
    if (!s) return o;
    for (; *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c); o += b; }
        else o += c;
    }
    return o;
}

static bool constTimeEquals(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    uint8_t diff = (uint8_t)(la != lb);
    for (size_t i = 0; i < la && i < lb; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0 && la > 0;
}

const char* CompanionServer::getApName() { return apName; }
const char* CompanionServer::getApPassword() { return apPassword; }
const char* CompanionServer::getPortalPin() { return portalPin; }
void CompanionServer::requestAp(bool on) { apRequest = on ? 1 : 0; }

// Per-device AP password: SHA-256(eFuse MAC) mapped to 10 chars [a-z0-9],
// generated once and kept in NVS so the user can note it down.
static void deriveApPassword(char* out /* >= 11 bytes */) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint8_t salt[16];
    memcpy(salt, mac, 6);
    memcpy(salt + 6, "WOSU-AP-PW", 10);
    uint8_t hash[32];
    mbedtls_sha256(salt, sizeof(salt), hash, 0);
    static const char ALPH[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 10; i++) out[i] = ALPH[hash[i] % 36];
    out[10] = 0;
}

void CompanionServer::loadOrCreateApPassword() {
    Preferences p;
    if (p.begin("kronos_wp", false)) {
        size_t n = p.getString("ap_pw", apPassword, sizeof(apPassword));
        if (n >= 10 && strlen(apPassword) == 10) {
            p.end();
            return;
        }
        deriveApPassword(apPassword);
        p.putString("ap_pw", apPassword);
        p.end();
        Serial.println("[WIFI] Generated per-device AP password (stored in NVS)");
    } else {
        deriveApPassword(apPassword);   // same result every boot, just not cached
    }
}

void CompanionServer::regeneratePin() {
    uint32_t v = esp_random() % 1000000UL;
    snprintf(portalPin, sizeof(portalPin), "%06lu", (unsigned long)v);
    nav::markDirty(nav::PG_APP_CONNECT);
}

// The hotspot is a session: it switches itself off after AP_IDLE_MS without
// a connected station (and no OTA in progress). A running SoftAP costs far
// more than everything else on the watch together.
static const uint32_t AP_IDLE_MS = 5UL * 60UL * 1000UL;
static uint32_t sApLastUseMs = 0;

void CompanionServer::startAp() {
    if (apRunning) return;
    regeneratePin();
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    // 11 dBm covers wrist to phone comfortably; 19.5 dBm only drained the
    // battery and stressed the AXP2101 rail during TX bursts.
    WiFi.setTxPower(WIFI_POWER_11dBm);
    IPAddress IP(192, 168, 4, 1);
    WiFi.softAPConfig(IP, IP, IPAddress(255, 255, 255, 0));
    bool ok = WiFi.softAP(apName, apPassword, 1, 0, 4);
    if (!ok) {
        delay(150);
        ok = WiFi.softAP(apName, apPassword, 1, 0, 4);
    }
    if (ok) {
        dnsServer.start(53, "*", IP);
        server.begin();
        apRunning = true;
        sApLastUseMs = millis();
        Serial.printf("[WIFI] Access point '%s' up at %s, portal PIN active\n", apName, IP.toString().c_str());
    } else {
        apRunning = false;
        WiFi.mode(WIFI_OFF);
        Serial.printf("[WIFI] Failed to start SoftAP '%s' (out of internal memory)\n", apName);
    }
}

void CompanionServer::stopAp() {
    server.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apRunning = false;
    portalPin[0] = 0;
    Serial.println("[WIFI] Access point stopped");
}

// ---- request guards --------------------------------------------------------
bool CompanionServer::checkOrigin() {
    if (!server.hasHeader("Origin")) return true;
    String origin = server.header("Origin");
    String allowed1 = String("http://") + WiFi.softAPIP().toString();
    String allowed2 = "http://192.168.4.1";
    if (origin == allowed1 || origin == allowed2) return true;
    server.send(403, "application/json", "{\"error\":\"origin\"}");
    return false;
}

bool CompanionServer::requirePin() {
    if (!checkOrigin()) return false;
    String pin;
    if (server.hasHeader("X-Pin")) pin = server.header("X-Pin");
    else if (server.hasArg("pin")) pin = server.arg("pin");
    pin.trim();
    if (portalPin[0] && constTimeEquals(pin.c_str(), portalPin)) return true;
    server.sendHeader("Cache-Control", "no-store");
    server.send(403, "application/json", "{\"error\":\"pin\"}");
    return false;
}

bool CompanionServer::checkJsonBody(size_t maxLen) {
    if (server.hasHeader("Content-Length")) {
        long cl = server.header("Content-Length").toInt();
        if (cl < 0 || (size_t)cl > maxLen) {
            server.send(413, "application/json", "{\"error\":\"too_large\"}");
            return false;
        }
    }
    if (server.hasArg("plain") && server.arg("plain").length() > maxLen) {
        server.send(413, "application/json", "{\"error\":\"too_large\"}");
        return false;
    }
    return true;
}

// Silent variant for upload callbacks (no response can be sent mid-upload).
static bool uploadPinOk(WebServer& srv, const char* pin) {
    if (srv.hasHeader("Origin")) {
        String origin = srv.header("Origin");
        if (origin != String("http://") + WiFi.softAPIP().toString() && origin != "http://192.168.4.1") return false;
    }
    String got;
    if (srv.hasHeader("X-Pin")) got = srv.header("X-Pin");
    else if (srv.hasArg("pin")) got = srv.arg("pin");
    got.trim();
    return pin[0] && constTimeEquals(got.c_str(), pin);
}

// ---- OTA -------------------------------------------------------------------
static void otaReset() {
    portENTER_CRITICAL(&sOtaMux);
    sOtaState = OTA_IDLE;
    sOtaHashHex[0] = 0;
    sOtaSize = 0;
    portEXIT_CRITICAL(&sOtaMux);
    sOtaFailed = false;
    sOtaPinOk = false;
    sOtaTarget = nullptr;
}

static void otaReject(const char* why) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) esp_ota_set_boot_partition(running);
    if (Update.isRunning()) Update.abort();
    Serial.printf("[OTA] Rejected (%s), boot partition restored\n", why);
    otaReset();
    nav::markDirty(nav::PG_SYSTEM);
}

void CompanionServer::handleApiOtaUploadData() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        sOtaFailed = false;
        sOtaPinOk = uploadPinOk(server, portalPin);
        if (!sOtaPinOk) { sOtaFailed = true; return; }
        if (sOtaState == OTA_PENDING) {
            Serial.println("[OTA] Upload refused: previous image awaits confirmation");
            sOtaFailed = true;
            return;
        }
        const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
        if (!next) {
            Serial.println("[OTA] No OTA partition available");
            sOtaFailed = true;
            return;
        }
        sOtaMaxSize = next->size;
        sOtaTarget = next;
        Serial.printf("[OTA] Upload start: %s -> %s (max %lu bytes)\n",
                      upload.filename.c_str(), next->label, (unsigned long)sOtaMaxSize);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
            sOtaFailed = true;
            return;
        }
        mbedtls_sha256_init(&sOtaSha);
        mbedtls_sha256_starts(&sOtaSha, 0);
        sOtaSize = 0;
        sOtaState = OTA_RECEIVING;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (sOtaFailed) return;
        if (sOtaSize + upload.currentSize > sOtaMaxSize) {
            Serial.println("[OTA] Image larger than OTA partition, aborting");
            Update.abort();
            mbedtls_sha256_free(&sOtaSha);
            sOtaFailed = true;
            return;
        }
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
            Update.abort();
            mbedtls_sha256_free(&sOtaSha);
            sOtaFailed = true;
            return;
        }
        mbedtls_sha256_update(&sOtaSha, upload.buf, upload.currentSize);
        sOtaSize += upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        if (sOtaFailed) return;
        uint8_t digest[32];
        mbedtls_sha256_finish(&sOtaSha, digest);
        mbedtls_sha256_free(&sOtaSha);
        // Update.end(true) validates the image header and points the boot
        // partition at the new image but does NOT restart. We immediately put
        // the boot pointer back on the running image, so a power loss during
        // the confirmation window never boots the unconfirmed firmware; the
        // new slot is only activated in otaServiceTimeout() after otaConfirm(true).
        if (Update.end(true)) {
            const esp_partition_t* running = esp_ota_get_running_partition();
            if (running) esp_ota_set_boot_partition(running);
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            portENTER_CRITICAL(&sOtaMux);
            memcpy(sOtaHashHex, hex, sizeof(sOtaHashHex));
            sOtaState = OTA_PENDING;
            portEXIT_CRITICAL(&sOtaMux);
            sOtaPendingSince = millis();
            sOtaDecision = -1;
            Serial.printf("[OTA] Image received: %lu bytes, sha256 %s - waiting for confirmation\n",
                          (unsigned long)sOtaSize, hex);
            nav::markDirty(nav::PG_SYSTEM);
        } else {
            Update.printError(Serial);
            sOtaFailed = true;
            otaReject("image invalid");
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (!sOtaFailed && sOtaState == OTA_RECEIVING) {
            mbedtls_sha256_free(&sOtaSha);
        }
        sOtaFailed = true;
        otaReject("upload aborted");
    }
}

void CompanionServer::handleApiOtaUploadDone() {
    if (!sOtaPinOk) {
        otaReset();
        server.send(403, "application/json", "{\"error\":\"pin\"}");
        return;
    }
    if (sOtaFailed || sOtaState != OTA_PENDING) {
        if (sOtaState == OTA_RECEIVING) otaReject("incomplete");
        else sOtaFailed = false;
        server.send(400, "application/json", "{\"status\":\"error\",\"error\":\"rejected\"}");
        return;
    }
    String j = "{\"status\":\"pending\",\"sha256\":\"";
    j += sOtaHashHex;
    j += "\",\"size\":"; j += sOtaSize;
    j += ",\"confirmTimeoutSec\":"; j += (uint32_t)(OTA_CONFIRM_MS / 1000);
    j += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

bool CompanionServer::otaPending(char* sha256Hex, size_t hexLen, uint32_t* sizeBytes) {
    bool pending;
    portENTER_CRITICAL(&sOtaMux);
    pending = (sOtaState == OTA_PENDING);
    if (pending) {
        if (sha256Hex && hexLen > 0) {
            strncpy(sha256Hex, sOtaHashHex, hexLen - 1);
            sha256Hex[hexLen - 1] = 0;
        }
        if (sizeBytes) *sizeBytes = sOtaSize;
    }
    portEXIT_CRITICAL(&sOtaMux);
    return pending;
}

void CompanionServer::otaConfirm(bool accept) {
    sOtaDecision = accept ? 1 : 0;
}

void CompanionServer::otaServiceTimeout(unsigned long now) {
    if (sOtaState != OTA_PENDING) return;
    int8_t decision = sOtaDecision;
    if (decision == 1) {
        sOtaDecision = -1;
        const esp_partition_t* target = sOtaTarget ? sOtaTarget : esp_ota_get_next_update_partition(NULL);
        esp_err_t err = target ? esp_ota_set_boot_partition(target) : ESP_ERR_NOT_FOUND;
        if (err != ESP_OK) {
            Serial.printf("[OTA] Activating new image failed (%d)\n", (int)err);
            otaReject("boot partition switch failed");
            return;
        }
        Serial.printf("[OTA] Confirmed on watch, restarting into %s\n", target->label);
        SettingsStorage::flushNow();
        delay(300);
        ESP.restart();
        return;
    }
    if (decision == 0) {
        sOtaDecision = -1;
        otaReject("declined on watch");
        return;
    }
    if (now - sOtaPendingSince > OTA_CONFIRM_MS) {
        otaReject("confirmation timeout");
    }
}

// ---- SD upload -------------------------------------------------------------
void CompanionServer::handleApiSdUploadData() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        sSdUploadActive = false;
        sSdUploadFailed = false;
        if (!uploadPinOk(server, portalPin)) { sSdUploadFailed = true; return; }
        if (!NavEngine::status().sdReady) {
            Serial.println("[SD] Upload refused: SD card not ready");
            sSdUploadFailed = true;
            return;
        }
        Serial.printf("[SD] Upload start: %s\n", upload.filename.c_str());
        if (!NavEngine::beginSdUpload("/kronos/germany.knav", 0)) {
            sSdUploadFailed = true;
            return;
        }
        sSdUploadActive = true;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (sSdUploadFailed || !sSdUploadActive) return;
        if (upload.totalSize + upload.currentSize > SD_UPLOAD_MAX) {
            Serial.println("[SD] Upload exceeds 64 MB limit, aborting");
            sSdUploadFailed = true;
            NavEngine::endSdUpload();
            sSdUploadActive = false;
            return;
        }
        if (!NavEngine::writeSdUploadChunk(upload.buf, upload.currentSize)) {
            sSdUploadFailed = true;
            NavEngine::endSdUpload();
            sSdUploadActive = false;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("[SD] Upload finished: %u bytes\n", upload.totalSize);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (sSdUploadActive) NavEngine::endSdUpload();
        sSdUploadActive = false;
        sSdUploadFailed = true;
    }
}

void CompanionServer::handleApiSdUploadDone() {
    bool ok = false;
    if (sSdUploadActive && !sSdUploadFailed) ok = NavEngine::endSdUpload();
    sSdUploadActive = false;
    if (!ok) {
        server.send(sSdUploadFailed ? 403 : 500, "application/json", "{\"status\":\"error\",\"error\":\"sd\"}");
        sSdUploadFailed = false;
        return;
    }
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ---------------------------------------------------------------------------
bool CompanionServer::init() {
    // Stable SSID derived from the factory MAC (WiFi.macAddress() is all-zero
    // before the radio starts).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(apName, sizeof(apName), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    loadOrCreateApPassword();

    static const char* HEADERS[] = {"X-Pin", "Origin", "Content-Length"};
    server.collectHeaders(HEADERS, 3);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/settings", HTTP_POST, handleApiSettings);
    server.on("/api/inbox", HTTP_GET, handleApiInbox);
    server.on("/api/time", HTTP_POST, handleApiTimeSync);
    server.on("/api/notify", HTTP_POST, handleApiNotification);
    server.on("/api/lora", HTTP_POST, handleApiLoraSend);
    server.on("/api/find", HTTP_POST, handleApiFind);
    server.on("/api/ailimits", HTTP_ANY, handleApiAiLimits);
    server.on("/api/calendar", HTTP_ANY, handleApiCalendar);
    server.on("/api/media", HTTP_ANY, handleApiMedia);
    server.on("/api/ota/status", HTTP_GET, handleApiOtaStatus);
    server.on("/api/ota/upload", HTTP_POST, handleApiOtaUploadDone, handleApiOtaUploadData);
    server.on("/api/sd/upload", HTTP_POST, handleApiSdUploadDone, handleApiSdUploadData);
    // captive portal: every unknown URL (OS connectivity checks) -> the app
    server.onNotFound([]() {
        if (server.uri().startsWith("/api/")) {
            server.send(404, "application/json", "{\"error\":\"not_found\"}");
            return;
        }
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
        server.send(302, "text/plain", "");
    });

    // Never started at boot: the user opens the hotspot from the watch when
    // the phone portal is needed (SettingsStorage resets wifiApEnabled).
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] Access point off (started on demand)");
    return true;
}

void CompanionServer::handleRoot() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", INDEX_HTML);
}

void CompanionServer::handleApiStatus() {
    if (!requirePin()) return;
    SystemSettings& s = SettingsStorage::get();
    uint32_t hist[7];
    MotionHaptic::getHistory(hist);
    MediaState ms = BleCompanion::getMediaState();
    String j;
    j.reserve(1100);
    j += "{\"ssid\":\"";      j += jsonEscape(apName);
    j += "\",\"ver\":\"";     j += OS_VERSION;
    j += "\",\"build\":\"";   j += OS_BUILD;
    j += "\",\"epoch\":";     j += (uint32_t)time(NULL);
    j += ",\"offset\":";      j += (int32_t)RtcManager::utcOffsetSec();
    j += ",\"zone\":\"";      j += jsonEscape(RtcManager::timezoneName());
    j += "\",\"syncSrc\":\""; j += jsonEscape(RtcManager::lastSyncSource());
    j += "\",\"syncAt\":";    j += (uint32_t)RtcManager::lastSyncEpoch();
    j += ",\"battery\":";     j += PowerManager::getBatteryPercent();
    j += ",\"voltage\":";     j += String(PowerManager::getBatteryVoltage(), 2);
    j += ",\"charging\":";    j += PowerManager::isCharging() ? "true" : "false";
    j += ",\"temp\":";        j += String(PowerManager::getTempCelsius(), 1);
    j += ",\"steps\":";       j += MotionHaptic::getStepsToday();
    j += ",\"distance\":";    j += String(MotionHaptic::getDistanceKm(), 2);
    j += ",\"kcal\":";        j += MotionHaptic::getCalories();
    j += ",\"goal\":";        j += s.dailyStepGoal;
    j += ",\"history\":[";
    for (int i = 0; i < 7; i++) { if (i) j += ','; j += hist[i]; }
    j += "],\"face\":";       j += s.watchfaceIndex % FACE_COUNT;
    j += ",\"accent\":";      j += s.accentColor;
    j += ",\"brightness\":";  j += s.brightness;
    j += ",\"sleep\":";       j += s.autoSleepSec;
    j += ",\"haptic\":";      j += s.hapticEnabled ? 1 : 0;
    j += ",\"sound\":";       j += s.soundEnabled ? 1 : 0;
    j += ",\"aod\":";         j += s.aodEnabled ? 1 : 0;
    j += ",\"wrist\":";       j += s.wristWakeEnabled ? 1 : 0;
    j += ",\"beacon\":";      j += s.loraBeaconEnabled ? 1 : 0;
    j += ",\"ble\":";         j += s.bleEnabled ? 1 : 0;
    j += ",\"gps_en\":";      j += s.gpsEnabled ? 1 : 0;
    j += ",\"lora_en\":";     j += s.loraEnabled ? 1 : 0;
    j += ",\"otaPending\":";  j += (sOtaState == OTA_PENDING) ? "true" : "false";
    j += ",\"media\":{\"playing\":"; j += ms.isPlaying ? "true" : "false";
    j += ",\"title\":\"";     j += jsonEscape(ms.title);
    j += "\",\"artist\":\"";  j += jsonEscape(ms.artist);
    j += "\",\"volume\":";    j += ms.volumePct;
    j += "},\"gps\":{\"online\":"; j += GpsManager::isOnline() ? "true" : "false";
    j += ",\"powered\":";     j += GpsManager::isPowered() ? "true" : "false";
    j += ",\"fix\":";         j += GpsManager::hasFix() ? "true" : "false";
    j += ",\"sats\":";        j += GpsManager::satellites();
    j += ",\"hasPos\":";      j += GpsManager::hasLastFix() ? "true" : "false";
    j += ",\"lat\":";         j += String(GpsManager::latitude(), 6);
    j += ",\"lon\":";         j += String(GpsManager::longitude(), 6);
    j += ",\"alt\":";         j += String(GpsManager::altitudeM(), 0);
    j += ",\"fixAt\":";       j += (uint32_t)GpsManager::lastFixEpoch();
    j += "},\"lora\":{\"ready\":"; j += RadioLoRa::isReady() ? "true" : "false";
    j += ",\"tx\":";          j += RadioLoRa::getSentCount();
    j += ",\"rx\":";          j += RadioLoRa::getRecvCount();
    j += ",\"peers\":";       j += RadioLoRa::getPeerCount();
    j += "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleApiSettings() {
    if (!requirePin()) return;
    SystemSettings& s = SettingsStorage::get();
    auto flag = [](const char* k, bool& field) {
        if (server.hasArg(k)) field = server.arg(k).toInt() != 0;
    };
    if (server.hasArg("face")) {
        long f = server.arg("face").toInt();
        if (f < 0) f = 0;
        if (f >= FACE_COUNT) f = FACE_COUNT - 1;
        s.watchfaceIndex = (uint8_t)f;
        nav::markDirty(nav::PG_FACE);
    }
    if (server.hasArg("color"))  s.accentColor = (uint16_t)constrain(server.arg("color").toInt(), 0, 65535);
    if (server.hasArg("bright")) {
        s.brightness = (uint8_t)constrain(server.arg("bright").toInt(), 10, 255);
        PowerManager::setBrightness(s.brightness);
    }
    if (server.hasArg("goal"))   s.dailyStepGoal = (uint32_t)constrain(server.arg("goal").toInt(), 500, 100000);
    if (server.hasArg("sleep"))  s.autoSleepSec = (uint16_t)constrain(server.arg("sleep").toInt(), 0, 600);
    flag("haptic", s.hapticEnabled);
    flag("sound",  s.soundEnabled);
    flag("aod",    s.aodEnabled);
    flag("wrist",  s.wristWakeEnabled);
    flag("beacon", s.loraBeaconEnabled);
    if (server.hasArg("ble"))     BleCompanion::setEnabled(server.arg("ble").toInt() != 0);
    if (server.hasArg("gps_en"))  GpsManager::setEnabled(server.arg("gps_en").toInt() != 0);
    if (server.hasArg("lora_en")) RadioLoRa::setEnabled(server.arg("lora_en").toInt() != 0);
    SettingsStorage::save();
    PowerManager::handleActivity();
    MotionHaptic::triggerHaptic(HAPTIC_CLICK);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiInbox() {
    if (!requirePin()) return;
    LoraMessage msgs[8];
    uint8_t n = RadioLoRa::getInbox(msgs, 8);
    String j = "[";
    for (uint8_t i = 0; i < n; i++) {
        if (i) j += ',';
        j += "{\"text\":\""; j += jsonEscape(msgs[i].text);
        j += "\",\"rssi\":"; j += String(msgs[i].rssi, 0);
        j += ",\"snr\":";    j += String(msgs[i].snr, 1);
        j += ",\"at\":";     j += msgs[i].epoch;
        j += '}';
    }
    j += ']';
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleApiTimeSync() {
    if (!requirePin()) return;
    if (!server.hasArg("epoch")) {
        server.send(400, "application/json", "{\"error\":\"missing_epoch\"}");
        return;
    }
    long long epl = atoll(server.arg("epoch").c_str());
    if (epl < 1700000000LL || epl > 4102444800LL) {   // 2023-11 .. 2100-01
        server.send(400, "application/json", "{\"error\":\"bad_epoch\"}");
        return;
    }
    time_t ep = (time_t)epl;
    int32_t tz = server.hasArg("tz") ? (int32_t)constrain(server.arg("tz").toInt(), -50400, 50400) : 0;
    String zone = server.hasArg("zone") ? server.arg("zone") : String("");
    if (zone.length() > 40) zone = zone.substring(0, 40);
    RtcManager::syncFromEpoch(ep, tz, zone.length() ? zone.c_str() : nullptr);
    RtcManager::noteSyncSource("PHONE");
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiNotification() {
    if (!requirePin()) return;
    String title = server.hasArg("title") ? server.arg("title") : "Handy";
    String msg = server.hasArg("msg") ? server.arg("msg") : "";
    if (title.length() > 40) title = title.substring(0, 40);
    if (msg.length() > 160) msg = msg.substring(0, 160);
    DisplayEngine::showNotification(title, msg, 6000, ICO_SMARTPHONE);
    AudioEngine::playSoundFx(SFX_NOTIFY);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiLoraSend() {
    if (!requirePin()) return;
    if (!RadioLoRa::isEnabled()) {
        server.send(409, "application/json", "{\"status\":\"disabled\"}");
        return;
    }
    bool ok = false;
    if (server.hasArg("sos")) ok = RadioLoRa::sendSosBeacon();
    else if (server.hasArg("msg")) {
        String m = server.arg("msg");
        if (m.length() > 70) m = m.substring(0, 70);
        ok = RadioLoRa::sendPacket(m);
    }
    server.send(ok ? 200 : 503, "application/json", ok ? "{\"status\":\"queued\"}" : "{\"status\":\"error\",\"error\":\"radio\"}");
}

void CompanionServer::handleApiFind() {
    if (!requirePin()) return;
    PowerManager::handleActivity();
    DisplayEngine::showNotification("Hier bin ich", "Vom Handy gesucht", 8000, ICO_SMARTPHONE);
    AudioEngine::playSoundFx(SFX_ALARM);
    MotionHaptic::triggerHaptic(HAPTIC_ALERT_LONG);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiAiLimits() {
    if (!requirePin()) return;
    if (server.method() == HTTP_POST) {
        if (!checkJsonBody(JSON_BODY_MAX)) return;
        bool ok = false;
        if (server.hasArg("plain")) {
            ok = AiLimitsStore::updateFromJson(server.arg("plain").c_str());
            if (ok) {
                nav::markDirty(nav::PG_AI_LIMITS);
                MotionHaptic::triggerHaptic(HAPTIC_SUCCESS);
            }
        }
        server.send(ok ? 200 : 400, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"error\":\"invalid_json\"}");
        return;
    }

    // GET
    AiLimitsData d = AiLimitsStore::get();
    String j = "{";
    j += "\"c_5h_p\":" + String(d.claude_5h_pct) + ",\"c_5h_r\":" + String(d.claude_5h_reset) + ",";
    j += "\"c_wk_p\":" + String(d.claude_wk_pct) + ",\"c_wk_r\":" + String(d.claude_wk_reset) + ",";
    j += "\"ag_g_5h_p\":" + String(d.ag_gemini_5h_pct) + ",\"ag_g_5h_r\":" + String(d.ag_gemini_5h_reset) + ",";
    j += "\"ag_g_wk_p\":" + String(d.ag_gemini_wk_pct) + ",\"ag_g_wk_r\":" + String(d.ag_gemini_wk_reset) + ",";
    j += "\"ag_c_5h_p\":" + String(d.ag_claude_5h_pct) + ",\"ag_c_5h_r\":" + String(d.ag_claude_5h_reset) + ",";
    j += "\"ag_c_wk_p\":" + String(d.ag_claude_wk_pct) + ",\"ag_c_wk_r\":" + String(d.ag_claude_wk_reset) + ",";
    j += "\"gc_5h_p\":" + String(d.gc_5h_pct) + ",\"gc_5h_r\":" + String(d.gc_5h_reset) + ",";
    j += "\"gc_d_p\":" + String(d.gc_daily_pct) + ",\"gc_d_r\":" + String(d.gc_daily_reset) + ",";
    j += "\"gc_wk_p\":" + String(d.gc_weekly_pct) + ",\"gc_wk_r\":" + String(d.gc_weekly_reset) + ",";
    j += "\"arg_p\":" + String(d.argon_weekly_pct) + ",\"arg_r\":" + String(d.argon_weekly_reset) + ",";
    j += "\"sync\":" + String(d.last_sync_time) + ",\"has_data\":" + (d.has_data ? "true" : "false");
    j += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleApiCalendar() {
    if (!requirePin()) return;
    if (server.method() == HTTP_POST) {
        if (!checkJsonBody(JSON_BODY_MAX)) return;
        if (server.hasArg("plain")) {
            BleCompanion::updateCalendarFromJson(server.arg("plain").c_str());
            server.send(200, "application/json", "{\"status\":\"ok\"}");
            return;
        }
        server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    // GET
    CalendarEvent events[12];
    uint8_t n = BleCompanion::getUpcomingEvents(events, 12);
    String j = "{\"events\":[";
    for (uint8_t i = 0; i < n; i++) {
        if (i > 0) j += ",";
        j += "{\"title\":\""; j += jsonEscape(events[i].title);
        j += "\",\"loc\":\""; j += jsonEscape(events[i].loc);
        j += "\",\"start\":"; j += events[i].startEpoch;
        j += ",\"end\":";     j += events[i].endEpoch;
        j += ",\"allDay\":";  j += events[i].allDay ? "true" : "false";
        j += "}";
    }
    j += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleApiMedia() {
    if (!requirePin()) return;
    if (server.method() == HTTP_POST) {
        String cmd = server.hasArg("cmd") ? server.arg("cmd") : "";
        MediaState ms = BleCompanion::getMediaState();
        if (cmd == "play_pause") {
            BleCompanion::setMediaPlaying(!ms.isPlaying);
            BleCompanion::sendMediaCommand("CMD:MEDIA:PLAY_PAUSE");
        } else if (cmd == "next") {
            BleCompanion::sendMediaCommand("CMD:MEDIA:NEXT");
        } else if (cmd == "prev") {
            BleCompanion::sendMediaCommand("CMD:MEDIA:PREV");
        } else if (cmd == "vol_up") {
            BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_UP");
            BleCompanion::setMediaVolume(ms.volumePct < 95 ? ms.volumePct + 5 : 100);
        } else if (cmd == "vol_down") {
            BleCompanion::sendMediaCommand("CMD:MEDIA:VOL_DOWN");
            BleCompanion::setMediaVolume(ms.volumePct > 5 ? ms.volumePct - 5 : 0);
        }

        if (server.hasArg("title") || server.hasArg("artist")) {
            ms = BleCompanion::getMediaState();
            String t = server.hasArg("title") ? server.arg("title") : String(ms.title);
            String a = server.hasArg("artist") ? server.arg("artist") : String(ms.artist);
            uint8_t vol = server.hasArg("vol") ? (uint8_t)constrain(server.arg("vol").toInt(), 0, 100) : ms.volumePct;
            bool play = server.hasArg("play") ? (server.arg("play") == "1" || server.arg("play") == "true") : ms.isPlaying;
            BleCompanion::setMediaState(play, t.c_str(), a.c_str(), vol);
        }
        nav::markDirty(nav::PG_APP_MEDIA);
        ms = BleCompanion::getMediaState();
        String j = "{\"status\":\"ok\",\"title\":\""; j += jsonEscape(ms.title); j += "\"}";
        server.send(200, "application/json", j);
        return;
    }
    // GET
    MediaState ms = BleCompanion::getMediaState();
    String j = "{\"isPlaying\":"; j += ms.isPlaying ? "true" : "false";
    j += ",\"title\":\"";  j += jsonEscape(ms.title);
    j += "\",\"artist\":\""; j += jsonEscape(ms.artist);
    j += "\",\"volume\":"; j += ms.volumePct;
    j += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleApiOtaStatus() {
    if (!requirePin()) return;
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    String j = "{\"os\":\""; j += OS_NAME;
    j += "\",\"version\":\""; j += OS_VERSION;
    j += "\",\"build\":\"";   j += OS_BUILD;
    j += "\",\"partition\":\""; j += jsonEscape(running ? running->label : "?");
    j += "\",\"nextPartition\":\""; j += jsonEscape(next ? next->label : "none");
    j += "\",\"maxImageSize\":"; j += (uint32_t)(next ? next->size : 0);
    j += ",\"freeSketchSpace\":"; j += (uint32_t)ESP.getFreeSketchSpace();
    j += ",\"pending\":"; j += (sOtaState == OTA_PENDING) ? "true" : "false";
    if (sOtaState == OTA_PENDING) {
        j += ",\"sha256\":\""; j += sOtaHashHex; j += "\",\"size\":"; j += sOtaSize;
    }
    j += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleClient() {
    unsigned long now = millis();
    otaServiceTimeout(now);

    int8_t req = apRequest;
    if (req >= 0) {
        apRequest = -1;
        if (req == 1 && !apRunning) startAp();
        else if (req == 0 && apRunning) stopAp();
    }
    if (!apRunning) return;
    dnsServer.processNextRequest();
    server.handleClient();

    // idle auto-off
    if (WiFi.softAPgetStationNum() > 0 || sOtaState != OTA_IDLE) {
        sApLastUseMs = now;
    } else if (now - sApLastUseMs >= AP_IDLE_MS) {
        stopAp();
        SettingsStorage::get().wifiApEnabled = false;
        nav::markDirty(nav::PG_CONTROL);
        nav::markDirty(nav::PG_APP_CONNECT);
        nav::markDirty(nav::PG_APP_SETTINGS);
        DisplayEngine::showToast("Hotspot aus", "5 Minuten ohne Verbindung", 2500, ICO_WIFI);
    }
}

bool CompanionServer::isApActive() { return apRunning; }
