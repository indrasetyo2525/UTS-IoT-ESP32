// UTS IoT - Monitoring Jarak Realtime
// ESP32-S3 + HC-SR04 + LCD 1602 I2C + WiFi provisioning (captive portal) + dashboard
//
// Library eksternal: LiquidCrystal_I2C (marcoschwartz)
// Library bawaan core ESP32: WiFi, WebServer, DNSServer, Preferences, Wire
//
// Catatan Arduino IDE: simpan file ini sebagai uts_iot/uts_iot.ino

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ====================== KONFIGURASI PIN ======================
#define PIN_TRIG 5
#define PIN_ECHO 6   // WAJIB lewat pembagi tegangan 5V -> 3.3V
#define PIN_SDA  8
#define PIN_SCL  9
#define LCD_ADDR 0x27  // ganti 0x3F jika LCD tidak menyala

// ====================== KONFIGURASI UMUM =====================
const char* AP_SSID = "ESP32-Setup";   // nama WiFi portal (tanpa password)
const byte DNS_PORT = 53;
const unsigned long SCAN_INTERVAL_MS   = 15000;  // scan WiFi tiap 15 detik
const unsigned long CONNECT_TIMEOUT_MS = 20000;
const unsigned long SAMPLE_INTERVAL_MS = 30;     // sampling sensor (~33 Hz)
const unsigned long LCD_INTERVAL_MS    = 250;

// ====================== OBJEK GLOBAL =========================
WebServer server(80);
DNSServer dns;
Preferences prefs;
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ---- sensor ----
const int WIN = 5;
float winBuf[WIN];
int winIdx = 0, winFilled = 0;
float distanceCm = -1;
bool distValid = false;
float ema = -1;

// ---- wifi ----
String netsJson = "[]";
int scanVer = 0;
bool scanning = false;
unsigned long lastScanStart = 0;
bool everScanned = false;

String pendSsid, pendPass;
bool wantConnect = false;
bool connecting = false;
bool connectFailed = false;
unsigned long connectStart = 0;

unsigned long lastSample = 0, lastLcd = 0;

// ====================== HALAMAN WEB ==========================
const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Setup WiFi</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0}
.wrap{max-width:480px;margin:0 auto;padding:20px}
.card{background:#1e293b;border-radius:16px;padding:20px}
h1{font-size:20px;margin:0 0 12px}
.badge{display:inline-block;padding:4px 10px;border-radius:999px;background:#334155;font-size:13px;margin-bottom:12px}
.badge.ok{background:#166534}.badge.err{background:#991b1b}
.list{max-height:240px;overflow-y:auto;border:1px solid #334155;border-radius:10px;margin:8px 0 14px}
.net{padding:10px 12px;border-bottom:1px solid #334155;cursor:pointer;display:flex;justify-content:space-between;gap:8px}
.net:last-child{border-bottom:0}.net:hover{background:#273449}
.sig{color:#38bdf8;font-family:monospace}
input{width:100%;padding:11px;margin-bottom:10px;border-radius:10px;border:1px solid #475569;background:#0f172a;color:#e2e8f0;font-size:16px}
button,a.btn{display:block;width:100%;text-align:center;padding:12px;border:0;border-radius:10px;background:#0284c7;color:#fff;font-size:16px;text-decoration:none;margin-top:6px;cursor:pointer}
button:disabled{opacity:.5}
a.btn.off{background:#475569;pointer-events:none;opacity:.6}
small{color:#94a3b8}
</style></head><body><div class="wrap"><div class="card">
<h1>Setup WiFi ESP32</h1>
<div id="badge" class="badge">Memeriksa...</div>
<div><small id="scanInfo">Daftar jaringan</small></div>
<div id="list" class="list"></div>
<input id="ssid" placeholder="Nama WiFi (SSID)" autocomplete="off">
<input id="pass" type="password" placeholder="Password (kosongkan jika terbuka)">
<button id="btn">Hubungkan</button>
<a id="dash" class="btn off" href="/dashboard">Buka Dashboard</a>
<p><small id="info"></small></p>
</div></div>
<script>
const $=id=>document.getElementById(id);
let ver=-1;
function bars(r){return r>-55?'▂▄▆█':r>-67?'▂▄▆_':r>-80?'▂▄__':'▂___';}
function render(nets){
  const l=$('list'),st=l.scrollTop;l.innerHTML='';
  nets.forEach(n=>{
    const d=document.createElement('div');d.className='net';
    const a=document.createElement('span');a.textContent=(n.e?'🔒 ':'')+n.s;
    const b=document.createElement('span');b.className='sig';b.textContent=bars(n.r);
    d.appendChild(a);d.appendChild(b);
    d.onclick=()=>{$('ssid').value=n.s;$('pass').focus();};
    l.appendChild(d);
  });
  l.scrollTop=st;
}
async function scan(){
  try{
    const j=await (await fetch('/scan',{cache:'no-store'})).json();
    if(j.ver!==ver){ver=j.ver;render(j.nets);}
    $('scanInfo').textContent=j.scanning?'Memindai jaringan...':'Daftar jaringan (diperbarui otomatis tiap 15 detik)';
  }catch(e){}
}
async function status(){
  try{
    const j=await (await fetch('/status',{cache:'no-store'})).json();
    const b=$('badge'),d=$('dash');
    if(j.connected){b.className='badge ok';b.textContent='Terhubung ke '+j.ssid+' (IP '+j.ip+')';d.className='btn';
      $('info').textContent='Dashboard juga bisa dibuka dari jaringan yang sama di http://'+j.ip+'/dashboard';}
    else if(j.connecting){b.className='badge';b.textContent='Menyambung...';d.className='btn off';}
    else if(j.failed){b.className='badge err';b.textContent='Gagal terhubung. Cek password.';d.className='btn off';}
    else{b.className='badge';b.textContent='Belum terhubung ke internet';d.className='btn off';}
  }catch(e){}
}
$('btn').onclick=async()=>{
  const s=$('ssid').value.trim();if(!s){alert('Isi SSID dulu');return;}
  $('btn').disabled=true;
  try{await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({ssid:s,pass:$('pass').value})});}catch(e){}
  $('btn').disabled=false;status();
};
scan();status();
setInterval(scan,3000);setInterval(status,1500);
</script></body></html>)rawliteral";

const char DASH_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dashboard Jarak</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0}
.wrap{max-width:560px;margin:0 auto;padding:20px}
.card{background:#1e293b;border-radius:16px;padding:20px;margin-bottom:16px}
h1{font-size:18px;margin:0 0 4px}
.big{font-size:64px;font-weight:700;text-align:center;margin:8px 0}
.unit{font-size:22px;color:#94a3b8}
.bar{height:14px;background:#334155;border-radius:7px;overflow:hidden}
#fill{height:100%;width:0;background:#38bdf8;transition:width .12s linear}
canvas{width:100%;height:160px;display:block}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#ef4444;margin-right:6px}
.dot.on{background:#22c55e}
small{color:#94a3b8}
</style></head><body><div class="wrap">
<div class="card"><h1>Dashboard Jarak Realtime</h1>
<small><span id="dot" class="dot"></span><span id="st">Menghubungkan...</span></small></div>
<div class="card">
<div class="big"><span id="val">--</span> <span class="unit">cm</span></div>
<div class="bar"><div id="fill"></div></div>
<small>Skala 0 - 200 cm</small></div>
<div class="card"><small>Grafik 12 detik terakhir</small><canvas id="cv"></canvas></div>
</div>
<script>
const $=id=>document.getElementById(id);
const MAXCM=200,N=120,hist=[];
const cv=$('cv'),ctx=cv.getContext('2d');
function resize(){const r=window.devicePixelRatio||1;cv.width=cv.clientWidth*r;cv.height=cv.clientHeight*r;ctx.setTransform(r,0,0,r,0,0);}
window.addEventListener('resize',resize);resize();
function draw(){
  const w=cv.clientWidth,h=cv.clientHeight;ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#334155';ctx.lineWidth=1;
  for(let i=0;i<=4;i++){const y=h*i/4;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke();}
  ctx.strokeStyle='#38bdf8';ctx.lineWidth=2;ctx.beginPath();let started=false;
  hist.forEach((v,i)=>{
    if(v===null){started=false;return;}
    const x=w*i/(N-1),y=h-(Math.min(v,MAXCM)/MAXCM)*h;
    if(!started){ctx.moveTo(x,y);started=true;}else ctx.lineTo(x,y);
  });
  ctx.stroke();
}
function online(on,txt){$('dot').className='dot'+(on?' on':'');$('st').textContent=txt;}
async function tick(){
  try{
    const r=await fetch('/api/distance',{cache:'no-store'});
    if(r.status===403){location.href='/';return;}
    const j=await r.json();
    if(j.valid){$('val').textContent=j.cm.toFixed(1);$('fill').style.width=Math.min(100,j.cm/MAXCM*100)+'%';hist.push(j.cm);online(true,'Online - data masuk');}
    else{$('val').textContent='--';hist.push(null);online(true,'Online - di luar jangkauan');}
  }catch(e){hist.push(null);online(false,'Terputus, mencoba lagi...');}
  while(hist.length>N)hist.shift();
  draw();setTimeout(tick,100);
}
tick();
</script></body></html>)rawliteral";

// ====================== SENSOR ===============================
float readOnceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long d = pulseIn(PIN_ECHO, HIGH, 25000UL);  // timeout ~4.3 m
  if (d == 0) return -1;
  return d * 0.0343f / 2.0f;
}

void sampleSensor() {
  winBuf[winIdx] = readOnceCm();
  winIdx = (winIdx + 1) % WIN;
  if (winFilled < WIN) winFilled++;

  float tmp[WIN];
  int n = 0;
  for (int i = 0; i < winFilled; i++) {
    if (winBuf[i] >= 2.0f && winBuf[i] <= 400.0f) tmp[n++] = winBuf[i];
  }
  if (n < 3) {  // data valid terlalu sedikit
    distValid = false;
    ema = -1;
    return;
  }
  for (int i = 1; i < n; i++) {  // insertion sort kecil
    float k = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }
  float med = tmp[n / 2];                       // median: buang lonjakan
  ema = (ema < 0) ? med : ema + 0.4f * (med - ema);  // EMA: haluskan
  distanceCm = ema;
  distValid = true;
}

// ====================== WIFI SCAN ============================
String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) continue;
    else o += c;
  }
  return o;
}

void finishScan(int n) {
  String out = "[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < i; j++) {
      if (WiFi.SSID(j) == s) { dup = true; break; }
    }
    if (dup) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"s\":\"" + jsonEscape(s) + "\",\"r\":" + String(WiFi.RSSI(i)) +
           ",\"e\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  out += "]";
  netsJson = out;
  scanVer++;
  WiFi.scanDelete();
  scanning = false;
}

void handleScanLoop() {
  bool staBusy = (WiFi.status() == WL_CONNECTED) || connecting;
  if (!scanning && !staBusy && (!everScanned || millis() - lastScanStart >= SCAN_INTERVAL_MS)) {
    WiFi.scanNetworks(true, true);  // async, tampilkan SSID tersembunyi
    scanning = true;
    everScanned = true;
    lastScanStart = millis();
  }
  if (scanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) finishScan(n);
    else if (n == WIFI_SCAN_FAILED) scanning = false;
  }
}

// ====================== WIFI CONNECT =========================
void handleConnectLoop() {
  if (wantConnect) {
    wantConnect = false;
    WiFi.disconnect();
    delay(50);
    WiFi.begin(pendSsid.c_str(), pendPass.c_str());
    connecting = true;
    connectFailed = false;
    connectStart = millis();
  }
  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting = false;
      connectFailed = false;
      Serial.print("Terhubung, IP: ");
      Serial.println(WiFi.localIP());
    } else if (millis() - connectStart > CONNECT_TIMEOUT_MS) {
      connecting = false;
      connectFailed = true;
      WiFi.disconnect();
      lastScanStart = 0;
      everScanned = false;  // scan ulang segera
    }
  }
}

// ====================== HTTP HANDLER =========================
bool isOnline() { return WiFi.status() == WL_CONNECTED; }

void handlePortal() { server.send_P(200, "text/html", PORTAL_HTML); }

void handleDashboard() {
  if (!isOnline()) {  // dashboard terkunci sampai ESP32 punya IP STA
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  server.send_P(200, "text/html", DASH_HTML);
}

void handleDistance() {
  if (!isOnline()) {
    server.send(403, "application/json", "{\"locked\":true}");
    return;
  }
  String j = "{\"valid\":";
  j += distValid ? "true" : "false";
  j += ",\"cm\":" + String(distValid ? distanceCm : 0.0f, 1);
  j += ",\"t\":" + String(millis()) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleScanApi() {
  String j = "{\"ver\":" + String(scanVer) + ",\"scanning\":" + (scanning ? "true" : "false") +
             ",\"nets\":" + netsJson + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleStatus() {
  String j = "{\"connected\":";
  j += isOnline() ? "true" : "false";
  j += ",\"connecting\":" + String(connecting ? "true" : "false");
  j += ",\"failed\":" + String(connectFailed ? "true" : "false");
  j += ",\"ssid\":\"" + jsonEscape(isOnline() ? WiFi.SSID() : String("")) + "\"";
  j += ",\"ip\":\"" + (isOnline() ? WiFi.localIP().toString() : String("")) + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleConnect() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  if (s.length() == 0) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  pendSsid = s;
  pendPass = p;
  prefs.putString("ssid", s);
  prefs.putString("pass", p);
  wantConnect = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNotFound() {  // captive portal: alihkan semua host asing ke portal
  String host = server.hostHeader();
  if (host == WiFi.softAPIP().toString() || host == WiFi.localIP().toString()) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

// ====================== LCD ==================================
void updateLcd() {
  char t[32], l0[17], l1[17];
  if (distValid) snprintf(t, sizeof t, "Jarak:%6.1f cm", distanceCm);
  else snprintf(t, sizeof t, "Jarak: -- cm");
  snprintf(l0, sizeof l0, "%-16.16s", t);

  if (isOnline()) snprintf(t, sizeof t, "%s", WiFi.localIP().toString().c_str());
  else if (connecting) snprintf(t, sizeof t, "Menyambung...");
  else snprintf(t, sizeof t, "AP:%s", WiFi.softAPIP().toString().c_str());
  snprintf(l1, sizeof l1, "%-16.16s", t);

  lcd.setCursor(0, 0);
  lcd.print(l0);
  lcd.setCursor(0, 1);
  lcd.print(l1);
}

// ====================== SETUP & LOOP =========================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("UTS IoT ESP32-S3");
  lcd.setCursor(0, 1);
  lcd.print("Memulai...");

  prefs.begin("wifi", false);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);  // latensi lebih rendah
  WiFi.setAutoReconnect(true);
  WiFi.softAP(AP_SSID);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  String savedSsid = prefs.getString("ssid", "");
  if (savedSsid.length() > 0) {  // sambung otomatis ke WiFi tersimpan
    WiFi.begin(savedSsid.c_str(), prefs.getString("pass", "").c_str());
    connecting = true;
    connectStart = millis();
  }

  dns.start(DNS_PORT, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handlePortal);
  server.on("/dashboard", HTTP_GET, handleDashboard);
  server.on("/api/distance", HTTP_GET, handleDistance);
  server.on("/scan", HTTP_GET, handleScanApi);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/connect", HTTP_POST, handleConnect);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  dns.processNextRequest();
  server.handleClient();

  unsigned long now = millis();
  if (now - lastSample >= SAMPLE_INTERVAL_MS) {
    lastSample = now;
    sampleSensor();
  }
  if (now - lastLcd >= LCD_INTERVAL_MS) {
    lastLcd = now;
    updateLcd();
  }

  handleConnectLoop();
  handleScanLoop();
}
