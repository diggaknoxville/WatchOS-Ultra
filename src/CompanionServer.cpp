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
#include <esp_mac.h>
#include <Update.h>

WebServer CompanionServer::server(HTTP_PORT);
DNSServer CompanionServer::dnsServer;
bool CompanionServer::apRunning = false;
char CompanionServer::apName[32] = {0};
volatile int8_t CompanionServer::apRequest = -1;

// ---------------------------------------------------------------------------
//  Offline single-page app (the watch network has no internet: no CDNs,
//  no web fonts). Same design language as the watch UI.
// ---------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = R"KRONOS(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#000000"><title>KRONOS</title>
<style>
:root{--bg:#000;--s1:#0f1218;--s2:#181c25;--st:#252a35;--t1:#f4f6fa;--t2:#9ba4b4;--t3:#5a6270;--ok:#30d158;--crit:#ff453a;--acc:#00f0ff}
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:var(--bg);color:var(--t1);font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;padding:20px 16px 48px;max-width:520px;margin:auto}
.cap{font-size:11px;letter-spacing:.18em;text-transform:uppercase;color:var(--t3)}
h2{font-size:12px;letter-spacing:.22em;text-transform:uppercase;color:var(--t2);font-weight:600;margin-bottom:16px;display:flex;align-items:center;gap:10px}
h2:before{content:"";width:18px;height:3px;border-radius:2px;background:var(--acc)}
.card{background:var(--s1);border:1px solid var(--st);border-radius:24px;padding:20px;margin-bottom:14px}
header{display:flex;justify-content:space-between;align-items:center;margin:4px 4px 18px}
.brand{font-weight:800;letter-spacing:.32em;font-size:18px}
.pill{font-size:11px;letter-spacing:.15em;padding:6px 12px;border-radius:99px;background:#30d15826;color:var(--ok);font-weight:700;white-space:nowrap}
.pill.off{background:#ff453a26;color:var(--crit)}
.clock{font-size:60px;font-weight:200;letter-spacing:-.02em;font-variant-numeric:tabular-nums;line-height:1.05}
.clock b{color:var(--acc);font-weight:200}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.tile{background:var(--s2);border-radius:16px;padding:12px 14px}
.v{font-size:20px;font-weight:600;font-variant-numeric:tabular-nums;margin-top:4px}
.btn{appearance:none;border:0;border-radius:16px;padding:15px;font:600 15px system-ui;width:100%;background:var(--acc);color:#000;cursor:pointer;display:block;text-align:center;text-decoration:none}
.btn.ghost{background:var(--s2);color:var(--t1);border:1px solid var(--st)}
.btn.red{background:var(--crit);color:#fff}
.btn:active{transform:scale(.98)}
.faces{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.face{background:var(--s2);border:2px solid transparent;border-radius:18px;padding:16px 6px;text-align:center;font-size:12px;letter-spacing:.14em;font-weight:700;cursor:pointer}
.face.on{border-color:var(--acc)}
.face i{display:block;font-style:normal;font-size:26px;margin-bottom:8px;color:var(--acc);letter-spacing:0}
.sw{display:flex;justify-content:space-between;align-items:center;padding:13px 0;border-bottom:1px solid var(--st)}
.sw:last-child{border:0}
.tg{width:52px;height:30px;border-radius:99px;background:var(--st);position:relative;transition:.2s;flex:none;cursor:pointer}
.tg:after{content:"";position:absolute;top:3px;left:3px;width:24px;height:24px;border-radius:50%;background:#fff;transition:.2s}
.tg.on{background:var(--acc)}.tg.on:after{left:25px}
input[type=range]{width:100%;accent-color:var(--acc);height:32px}
input[type=text],input[type=number],select{width:100%;background:var(--s2);border:1px solid var(--st);color:var(--t1);border-radius:14px;padding:13px;font:15px system-ui;margin-bottom:10px;outline:none}
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
#toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,140px);background:var(--s2);border:1px solid var(--st);padding:12px 22px;border-radius:99px;transition:.3s;font-size:14px;white-space:nowrap}
#toast.show{transform:translate(-50%,0)}
</style></head><body>
<header><div><div class="brand">KRONOS</div><div class="cap" id="ssid"></div></div><div class="pill" id="conn">CONNECTING</div></header>

<div class="card">
 <div class="row"><div class="cap">Watch time</div><div class="cap" id="sync"></div></div>
 <div class="clock" id="clock">--:--:--</div>
 <div class="cap" id="tz" style="margin:10px 0 16px"></div>
 <button class="btn ghost" onclick="syncTime(1)">Sync time from phone</button>
</div>

<div class="card"><h2>Status</h2><div class="grid">
 <div class="tile"><div class="cap">Battery</div><div class="v" id="bat">--</div></div>
 <div class="tile"><div class="cap">Voltage</div><div class="v" id="volt">--</div></div>
 <div class="tile"><div class="cap">PMU</div><div class="v" id="temp">--</div></div>
</div></div>

<div class="card"><h2>Activity</h2>
 <div class="row"><div><div class="clock" style="font-size:46px" id="steps">0</div><div class="cap" id="goaltxt"></div></div>
 <div style="text-align:right"><div class="v" id="dist">0 km</div><div class="cap" id="kcal"></div></div></div>
 <div class="bars" id="bars"></div><div class="dl cap" id="dl"></div>
</div>

<div class="card"><h2>Watch face</h2>
 <div class="faces" id="faces"></div>
 <div class="cap lbl">Accent colour</div><div class="dots" id="dots"></div>
 <div class="cap lbl">Brightness</div>
 <input type="range" id="bright" min="10" max="255" onchange="set('bright',this.value)">
</div>

<div class="card"><h2>Settings</h2><div id="toggles"></div>
 <div class="cap lbl">Daily step goal</div>
 <input type="number" id="goal" min="1000" max="50000" step="500" onchange="set('goal',this.value)">
 <div class="cap" style="margin:4px 0 8px">Screen timeout</div>
 <select id="sleep" onchange="set('sleep',this.value)"><option value="10">10 s</option><option value="15">15 s</option><option value="25">25 s</option><option value="45">45 s</option><option value="60">1 min</option><option value="120">2 min</option><option value="0">Never (Always On)</option></select>
</div>

<div class="card"><h2>Position</h2>
 <div class="row"><div id="pos" style="font-size:17px;font-variant-numeric:tabular-nums">No position yet</div><div class="pill off" id="gps">--</div></div>
 <div class="cap" id="posmeta" style="margin:10px 0 14px"></div>
 <a id="maplink" class="btn ghost" style="display:none">Open in maps</a>
</div>

<div class="card"><h2>LoRa mesh</h2>
 <div id="inbox" class="cap">No messages</div>
 <input type="text" id="lmsg" placeholder="Message" maxlength="70" style="margin-top:16px">
 <div class="grid" style="grid-template-columns:2fr 1fr"><button class="btn" onclick="lora()">Transmit</button><button class="btn red" onclick="sos()">SOS</button></div>
</div>

<div class="card"><h2>Notify watch</h2>
 <input type="text" id="nt" placeholder="Title" maxlength="40">
 <input type="text" id="nm" placeholder="Message" maxlength="120">
 <button class="btn" onclick="notify()">Send to watch</button>
</div>

<div class="card"><button class="btn ghost" onclick="post('/api/find');toast('Your watch is ringing')">Find my watch</button></div>
<div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const FACES=[["MODULAR","▦"],["CHRONO","◷"],["MINIMAL","‖"],["WAYFINDER","🧭"],["TOURBILLON","⚙"],["CASIO","⌚"],["BAUHAUS","◻"],["CYBERPUNK","⚡"],["WORDCLOCK","🔤"],["NEON","🌅"],["PILOT","✈"],["DIVER","⚓"],["RADAR","📡"],["LIFETIME","⏳"]];
const COLORS=["#00ffff","#30d158","#ffd60a","#ff9f0a","#ff453a","#ff2d92","#bf5af2","#5e5ce6","#64d2ff","#ffffff"];
const TOG=[["ble","Bluetooth (BLE)"],["gps_en","GPS / GNSS"],["lora_en","LoRa Radio"],["haptic","Haptics"],["sound","Sound"],["aod","Always-on display"],["wrist","Raise to wake"],["beacon","LoRa beacon"]];
let S=null,base=0,at=0;
function toast(m){const t=$('toast');t.textContent=m;t.classList.add('show');clearTimeout(t._h);t._h=setTimeout(()=>t.classList.remove('show'),2200)}
function post(u){return fetch(u,{method:'POST'})}
function set(k,v){post(`/api/settings?${k}=${encodeURIComponent(v)}`).then(load)}
function hex565(h){const n=parseInt(h.slice(1),16),r=n>>16,g=(n>>8)&255,b=n&255;return((r>>3)<<11)|((g>>2)<<5)|(b>>3)}
function c565(c){let r=(c>>11)&31,g=(c>>5)&63,b=c&31;r=r<<3|r>>2;g=g<<2|g>>4;b=b<<3|b>>2;return'#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('')}
function ago(t){if(!t)return'never';const d=Date.now()/1000-t;return d<90?'just now':d<3600?Math.round(d/60)+' min ago':d<172800?Math.round(d/3600)+' h ago':Math.round(d/86400)+' d ago'}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function syncTime(manual){const z=Intl.DateTimeFormat().resolvedOptions().timeZone||'';
 post(`/api/time?epoch=${Math.round(Date.now()/1000)}&tz=${-new Date().getTimezoneOffset()*60}&zone=${encodeURIComponent(z)}`).then(()=>{if(manual)toast('Time synchronised');load()})}
function lora(){const m=$('lmsg').value.trim();if(!m)return;post('/api/lora?msg='+encodeURIComponent(m)).then(()=>{$('lmsg').value='';toast('Transmitting on 868 MHz')})}
function sos(){if(confirm('Broadcast an SOS beacon with your position?'))post('/api/lora?sos=1').then(()=>toast('SOS beacon sent'))}
function notify(){post(`/api/notify?title=${encodeURIComponent($('nt').value||'Phone')}&msg=${encodeURIComponent($('nm').value)}`).then(()=>{toast('Delivered');$('nm').value=''})}
function tick(){if(!S)return;const d=new Date((base+S.offset)*1000+(Date.now()-at)),p=n=>String(n).padStart(2,'0');
 $('clock').innerHTML=`${p(d.getUTCHours())}<b>:</b>${p(d.getUTCMinutes())}<b>:</b>${p(d.getUTCSeconds())}`}
function render(){
 document.documentElement.style.setProperty('--acc',c565(S.accent));
 const o=S.offset/3600;$('ssid').textContent=S.ssid;$('tz').textContent=`${S.zone} · UTC${o>=0?'+':''}${o}`;
 $('sync').textContent=`${S.syncSrc} · ${ago(S.syncAt)}`;
 $('bat').textContent=S.battery+'%'+(S.charging?' +':'');$('volt').textContent=S.voltage.toFixed(2)+' V';$('temp').textContent=Math.round(S.temp)+'°';
 $('steps').textContent=S.steps.toLocaleString();$('goaltxt').textContent=`${Math.round(S.steps*100/S.goal)}% of ${S.goal.toLocaleString()} steps`;
 $('dist').textContent=S.distance.toFixed(2)+' km';$('kcal').textContent=S.kcal+' kcal';
 const pk=Math.max(S.goal,...S.history),days='SMTWTFS',wd=new Date((base+S.offset)*1000).getUTCDay();
 $('bars').innerHTML=S.history.map((v,i)=>`<div class="bar${i==6?' today':''}" style="height:${Math.max(4,v/pk*100)}%"></div>`).join('');
 $('dl').innerHTML=S.history.map((v,i)=>`<span>${days[(wd+1+i)%7]}</span>`).join('');
 $('faces').innerHTML=FACES.map((f,i)=>`<div class="face${S.face==i?' on':''}" onclick="set('face',${i})"><i>${f[1]}</i>${f[0]}</div>`).join('');
 $('dots').innerHTML=COLORS.map(c=>`<div class="dot${hex565(c)==S.accent?' on':''}" style="background:${c}" onclick="set('color',${hex565(c)})"></div>`).join('')+
  `<label class="dot" style="background:conic-gradient(red,yellow,lime,cyan,blue,magenta,red)"><input type="color" style="opacity:0;width:0;height:0" onchange="set('color',hex565(this.value))"></label>`;
 if(document.activeElement!==$('bright'))$('bright').value=S.brightness;
 if(document.activeElement!==$('goal'))$('goal').value=S.goal;
 $('sleep').value=S.sleep;
 $('toggles').innerHTML=TOG.map(t=>`<div class="sw"><span>${t[1]}</span><div class="tg${S[t[0]]?' on':''}" onclick="set('${t[0]}',${S[t[0]]?0:1})"></div></div>`).join('');
 const g=S.gps;$('gps').textContent=!g.online?'NO GNSS':g.fix?'3D FIX':g.powered?'SEARCHING':'STANDBY';$('gps').className='pill'+(g.fix?'':' off');
 if(g.hasPos){$('pos').textContent=`${Math.abs(g.lat).toFixed(5)}° ${g.lat>=0?'N':'S'}   ${Math.abs(g.lon).toFixed(5)}° ${g.lon>=0?'E':'W'}`;
  $('posmeta').textContent=`${g.sats} satellites · ${Math.round(g.alt)} m · fix ${ago(g.fixAt)}`;
  const l=$('maplink');l.style.display='block';l.href=`geo:${g.lat},${g.lon}?q=${g.lat},${g.lon}`}
 else $('posmeta').textContent=g.powered?'Searching for satellites (needs open sky)':'Tap "Locate" in the watch control centre';
 tick();
}
function load(){fetch('/api/status').then(r=>r.json()).then(d=>{S=d;base=d.epoch;at=Date.now();render();$('conn').className='pill';$('conn').textContent='LIVE'})
 .catch(()=>{$('conn').className='pill off';$('conn').textContent='OFFLINE'});
 fetch('/api/inbox').then(r=>r.json()).then(m=>{$('inbox').innerHTML=m.length?m.map(x=>`<div class="msg"><div>${esc(x.text)}</div>${ago(x.at)} · ${x.rssi} dBm · SNR ${x.snr}</div>`).join(''):'No messages yet'}).catch(()=>{})}
setInterval(tick,250);syncTime(0);load();setInterval(load,3000);
</script></body></html>)KRONOS";

// ---------------------------------------------------------------------------
static String jsonEscape(const char* s) {
    String o;
    for (; *s; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += c;
    }
    return o;
}

const char* CompanionServer::getApName() { return apName; }
void CompanionServer::requestAp(bool on) { apRequest = on ? 1 : 0; }

void CompanionServer::startAp() {
    if (apRunning) return;
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    IPAddress IP(192, 168, 4, 1);
    WiFi.softAPConfig(IP, IP, IPAddress(255, 255, 255, 0));
    bool ok = WiFi.softAP(apName, AP_PASSWORD, 1, 0, 4);
    if (!ok) {
        delay(100);
        ok = WiFi.softAP(apName, AP_PASSWORD, 1, 0, 4);
    }
    dnsServer.start(53, "*", IP);
    server.begin();
    apRunning = true;
    Serial.printf("[WIFI] Access point '%s' up at %s (ok=%d)\n", apName, IP.toString().c_str(), ok);
}

void CompanionServer::stopAp() {
    server.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apRunning = false;
    Serial.println("[WIFI] Access point stopped");
}

bool CompanionServer::init() {
    // Stable SSID derived from the factory MAC (WiFi.macAddress() is all-zero
    // before the radio starts).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(apName, sizeof(apName), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/settings", HTTP_POST, handleApiSettings);
    server.on("/api/inbox", HTTP_GET, handleApiInbox);
    server.on("/api/time", HTTP_POST, handleApiTimeSync);
    server.on("/api/notify", HTTP_POST, handleApiNotification);
    server.on("/api/lora", HTTP_POST, handleApiLoraSend);
    server.on("/api/find", HTTP_POST, handleApiFind);
    server.on("/api/ailimits", HTTP_ANY, handleApiAiLimits);
    server.on("/api/ota/status", HTTP_GET, handleApiOtaStatus);
    server.on("/api/ota/upload", HTTP_POST, [](){
        bool err = Update.hasError();
        server.send(200, "application/json", err ? "{\"status\":\"error\"}" : "{\"status\":\"ok\"}");
        if (!err) {
            delay(500);
            ESP.restart();
        }
    }, [](){
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("[OTA] Upload Start: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("[OTA] Upload Success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        }
    });
    server.on("/api/sd/upload", HTTP_POST, []() {
        bool ok = NavEngine::endSdUpload();
        server.send(200, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"status\":\"error\"}");
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("[SD] Upload Start: %s\n", upload.filename.c_str());
            NavEngine::beginSdUpload("/kronos/germany.knav", 0);
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            NavEngine::writeSdUploadChunk(upload.buf, upload.currentSize);
        } else if (upload.status == UPLOAD_FILE_END) {
            Serial.printf("[SD] Upload Finished: %u bytes\n", upload.totalSize);
        }
    });
    // captive portal: every unknown URL (OS connectivity checks) -> the app
    server.onNotFound([]() {
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
        server.send(302, "text/plain", "");
    });

    if (SettingsStorage::get().wifiApEnabled) {
        startAp();
    } else {
        WiFi.mode(WIFI_OFF);
        Serial.println("[WIFI] Access point disabled by user settings");
    }
    return true;
}

void CompanionServer::handleRoot() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", INDEX_HTML);
}

void CompanionServer::handleApiStatus() {
    SystemSettings& s = SettingsStorage::get();
    uint32_t hist[7];
    MotionHaptic::getHistory(hist);
    String j;
    j.reserve(900);
    j += "{\"ssid\":\"";      j += apName;
    j += "\",\"epoch\":";     j += (uint32_t)time(NULL);
    j += ",\"offset\":";      j += RtcManager::utcOffsetSec();
    j += ",\"zone\":\"";      j += jsonEscape(RtcManager::timezoneName());
    j += "\",\"syncSrc\":\""; j += RtcManager::lastSyncSource();
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
    j += ",\"gps\":{\"online\":"; j += GpsManager::isOnline() ? "true" : "false";
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
    j += "}}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleApiSettings() {
    SystemSettings& s = SettingsStorage::get();
    auto flag = [](const char* k, bool& field) {
        if (server.hasArg(k)) field = server.arg(k).toInt() != 0;
    };
    if (server.hasArg("face")) {
        s.watchfaceIndex = (uint8_t)(server.arg("face").toInt() % FACE_COUNT);
        nav::markDirty(nav::PG_FACE);
    }
    if (server.hasArg("color"))  s.accentColor = (uint16_t)server.arg("color").toInt();
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
    if (!server.hasArg("epoch")) {
        server.send(400, "application/json", "{\"error\":\"missing_epoch\"}");
        return;
    }
    time_t ep = (time_t)server.arg("epoch").toInt();
    int32_t tz = server.hasArg("tz") ? server.arg("tz").toInt() : 0;
    String zone = server.hasArg("zone") ? server.arg("zone") : String("");
    RtcManager::syncFromEpoch(ep, tz, zone.length() ? zone.c_str() : nullptr);
    RtcManager::noteSyncSource("PHONE");
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiNotification() {
    String title = server.hasArg("title") ? server.arg("title") : "Phone";
    String msg = server.hasArg("msg") ? server.arg("msg") : "";
    DisplayEngine::showNotification(title, msg, 6000);
    AudioEngine::playSoundFx(SFX_NOTIFY);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiLoraSend() {
    bool ok = false;
    if (server.hasArg("sos")) ok = RadioLoRa::sendSosBeacon();
    else if (server.hasArg("msg")) ok = RadioLoRa::sendPacket(server.arg("msg"));
    server.send(ok ? 200 : 503, "application/json", ok ? "{\"status\":\"queued\"}" : "{\"error\":\"radio\"}");
}

void CompanionServer::handleApiFind() {
    PowerManager::handleActivity();
    DisplayEngine::showNotification("Here I am", "Ringing from your phone", 8000);
    AudioEngine::playSoundFx(SFX_ALARM);
    MotionHaptic::triggerHaptic(HAPTIC_ALERT_LONG);
    server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void CompanionServer::handleApiAiLimits() {
    if (server.method() == HTTP_POST) {
        bool ok = false;
        if (server.hasArg("plain")) {
            ok = AiLimitsStore::updateFromJson(server.arg("plain").c_str());
            if (ok) nav::markDirty(nav::PG_AI_LIMITS);
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

void CompanionServer::handleApiOtaStatus() {
    String j = "{\"version\":\"1.0.0\",\"os\":\"KRONOS OS\",\"build\":\"2026.10\",\"repo\":\"diggaknoxville/WatchOS-Ultra\",\"release_url\":\"https://api.github.com/repos/diggaknoxville/WatchOS-Ultra/releases/latest\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

void CompanionServer::handleClient() {
    int8_t req = apRequest;
    if (req >= 0) {
        apRequest = -1;
        if (req == 1 && !apRunning) startAp();
        else if (req == 0 && apRunning) stopAp();
    }
    if (!apRunning) return;
    dnsServer.processNextRequest();
    server.handleClient();
}

bool CompanionServer::isApActive() { return apRunning; }
