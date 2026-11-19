/******************************************************
 * UTS IoT - ESP32-S3 + HC-SR04   (versi optimasi v2)
 * Smart WiFi Provisioning (Captive Portal) + Distance Monitoring
 *
 * Library tambahan : "LiquidCrystal I2C"
 * Wiring LCD 1602  : SDA -> GPIO8, SCL -> GPIO9, VCC -> 5V
 * Wiring HC-SR04   : TRIG -> GPIO5, ECHO -> GPIO18
 *                    ECHO WAJIB lewat pembagi tegangan 5V -> 3.3V
 *
 * Alur:
 *  1. ESP32 selalu memancarkan AP "ESP32-CONFIG-23053001" (192.168.4.1)
 *  2. HP buka 192.168.4.1 (atau otomatis lewat captive portal)
 *     -> halaman setup: daftar WiFi + RSSI (diperbarui otomatis tiap 15 detik),
 *        form SSID/password, tombol Simpan
 *  3. Kredensial disimpan di Preferences, ESP32 menyambung (STA)
 *     tanpa restart, AP tetap aktif
 *  4. Dashboard jarak real-time: http://<IP-STA>/  atau  http://192.168.4.1/dashboard
 *     !! Dashboard TERKUNCI sampai ESP32 benar-benar terhubung WiFi (ada IP STA) !!
 *
 * Perubahan v2:
 *  - Scan WiFi otomatis tiap 15 detik, hasil di-cache di ESP32, halaman setup
 *    memperbarui daftar otomatis (jaringan baru langsung muncul) tanpa reload,
 *    posisi scroll & pilihan tidak hilang.
 *  - Pull-to-refresh (geser ke bawah = refresh) dimatikan di semua halaman.
 *  - Dashboard & /data dikunci jika staState != ONLINE.
 *  - Sensor: 25 Hz, trimmed-mean 5 sampel, filter adaptif lebih responsif.
 ******************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <esp_timer.h>
#include <driver/gpio.h>

/*************** PIN & KONFIGURASI ***************/

#define TRIG_PIN 5
#define ECHO_PIN 18
#define LED_PIN 2
#define LCD_SDA 8
#define LCD_SCL 9
#define RESET_BTN_PIN 0 // tombol BOOT; tahan 3 detik = reset konfigurasi WiFi

const char *AP_SSID = "ESP32-CONFIG-23053001";

// --- Sensor ---
// KALIBRASI (penting untuk akurasi "jarak asli"):
//  1. Set TEMP_C sesuai suhu ruangan.
//  2. Letakkan objek datar tegak lurus di 10 cm, 30 cm, 100 cm (ukur pakai penggaris).
//  3. Set CAL_DEBUG = true, buka Serial Monitor (115200), baca nilai RAW yang stabil
//     di tiap jarak, lalu isi tabel CAL_RAW (terbaca) dan CAL_REAL (penggaris).
//     Firmware menginterpolasi linear antar titik -> koreksi di setiap jarak 1-100 cm.
const float TEMP_C = 28.0f;      // suhu udara sekitar (C), sesuaikan
const float HUMIDITY = 60.0f;    // kelembapan udara (%RH), sesuaikan
const float CM_PER_US = (331.3f + 0.606f * TEMP_C + 0.0124f * HUMIDITY) / 10000.0f; // kecepatan suara terkoreksi suhu & kelembapan

const bool CAL_DEBUG = false;    // true = cetak RAW & REAL ke Serial tiap 0.5 detik
const int CAL_N = 6;
//                              titik kalibrasi (cm)
const float CAL_RAW[CAL_N]  = {1.0f, 5.0f, 10.0f, 30.0f, 60.0f, 100.0f}; // yang terbaca sensor
const float CAL_REAL[CAL_N] = {1.0f, 5.0f, 10.0f, 30.0f, 60.0f, 100.0f}; // jarak asli (penggaris)

// Echo "tanpa objek" dari HC-SR04 = ~38 ms, jadi periode harus >= 40 ms
// agar ping berikutnya tidak bertabrakan dengan echo sebelumnya.
const uint32_t PING_PERIOD_MS = 40;  // 25 pengukuran/detik
const uint32_t ECHO_TIMEOUT_MS = 26; // sekitar 4.4 m
const float MIN_CM = 1.0f;
const float MAX_CM = 400.0f;
const int SAMPLE = 9;                // jendela filter (lebih banyak = lebih presisi saat diam)

/*************** GLOBAL ***************/

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

String wifiSSID = "";
String wifiPASS = "";

enum StaState
{
    STA_IDLE,       // belum ada kredensial
    STA_CONNECTING, // sedang menyambung
    STA_ONLINE,     // terhubung
    STA_LOST,       // sempat terhubung lalu terputus
    STA_FAILED      // gagal menyambung
};

StaState staState = STA_IDLE;
unsigned long staConnectStart = 0;
unsigned long notifUntil = 0; // LCD menampilkan notifikasi "WiFi Terhubung" sampai waktu ini
unsigned long restartAt = 0;  // jadwal restart (non-blocking)

// --- Scan WiFi otomatis ---
const unsigned long AUTO_SCAN_MS = 15000;
unsigned long lastAutoScan = 0;
unsigned long lastScanReq = 0; // kapan halaman setup terakhir meminta /scan
bool scanActive = false;       // scan async sedang berjalan (dimulai oleh kita)
String scanList = "[]";        // cache hasil scan (array JSON)
uint32_t scanSeq = 0;          // naik setiap ada hasil scan baru

/*************** LCD 1602 (STATUS ESP SAJA) ****************/

LiquidCrystal_I2C *lcd = nullptr;
String lcdCache[2];
uint8_t lcdPage = 0;
unsigned long lastPageMs = 0;
unsigned long lastLcdMs = 0;

void lcdLine(uint8_t row, String text)
{
    if (!lcd)
        return;

    if (text.length() > 16)
        text = text.substring(0, 16);

    while (text.length() < 16)
        text += ' ';

    if (text == lcdCache[row]) // hanya tulis kalau berubah
        return;

    lcdCache[row] = text;
    lcd->setCursor(0, row);
    lcd->print(text);
}

void lcdBegin()
{
    Wire.begin(LCD_SDA, LCD_SCL);

    const uint8_t candidates[] = {0x27, 0x3F};

    for (int i = 0; i < 2 && !lcd; i++)
    {
        Wire.beginTransmission(candidates[i]);

        if (Wire.endTransmission() == 0)
            lcd = new LiquidCrystal_I2C(candidates[i], 16, 2);
    }

    if (!lcd)
    {
        Serial.println("LCD I2C tidak ditemukan (cek wiring)");
        return;
    }

    lcd->init();
    lcd->backlight();
    lcd->clear();

    lcdCache[0] = "";
    lcdCache[1] = "";

    lcdLine(0, "UTS IoT ESP32-S3");
    lcdLine(1, "Memulai...");
}

void lcdTask()
{
    if (!lcd || restartAt)
        return;

    if (millis() - lastLcdMs < 200)
        return;

    lastLcdMs = millis();

    // Notifikasi saat WiFi baru terhubung (5 detik)
    if ((long)(notifUntil - millis()) > 0)
    {
        lcdLine(0, "WiFi Terhubung!");
        lcdLine(1, WiFi.localIP().toString());
        return;
    }

    // Halaman 2 (status STA) hanya ada kalau sudah ada WiFi tersimpan
    uint8_t pages = (wifiSSID != "") ? 2 : 1;

    if (pages == 1)
        lcdPage = 0;
    else if (millis() - lastPageMs >= 3000)
    {
        lastPageMs = millis();
        lcdPage = !lcdPage;
    }

    if (lcdPage == 0)
    {
        // Status Access Point + IP + jumlah HP yang terhubung
        lcdLine(0, "Mode AP: AKTIF");
        lcdLine(1, WiFi.softAPIP().toString() + " [" + String(WiFi.softAPgetStationNum()) + "]");
    }
    else
    {
        // Status Station (WiFi router/hotspot)
        switch (staState)
        {
        case STA_ONLINE:
            lcdLine(0, "STA Terhubung");
            lcdLine(1, WiFi.localIP().toString());
            break;
        case STA_CONNECTING:
            lcdLine(0, "Menyambung WiFi");
            lcdLine(1, wifiSSID);
            break;
        case STA_FAILED:
            lcdLine(0, "Gagal Konek WiFi");
            lcdLine(1, "IP: -");
            break;
        default:
            lcdLine(0, "WiFi Terputus");
            lcdLine(1, "IP: -");
            break;
        }
    }
}

/*************** HC-SR04 (task terpisah + interrupt) ****************/
// Sensor berjalan di FreeRTOS task sendiri (core 1, prioritas lebih tinggi
// dari loop). Web server, LCD, scan WiFi tidak mengganggu jadwal pengukuran.
// Waktu echo dicatat di ISR (presisi mikrodetik), bukan pulseIn() yang memblokir.
//
// Filter: trimmed-mean 5 sampel (urutkan, buang terkecil & terbesar, rata-rata
// 3 tengah -> tahan lonjakan dan lebih halus dari median murni) -> penghalusan
// adaptif (diam saat stabil, langsung loncat saat objek bergerak jauh).

volatile float gDistance = -1; // -1 = tidak terdeteksi

// Kalibrasi multi-titik: interpolasi linear antar titik CAL_RAW -> CAL_REAL
// (di luar rentang tabel dipakai ekstrapolasi dari segmen ujung)
float calibrate(float raw)
{
    int i = 0;

    while (i < CAL_N - 2 && raw > CAL_RAW[i + 1])
        i++;

    float x0 = CAL_RAW[i], x1 = CAL_RAW[i + 1];
    float y0 = CAL_REAL[i], y1 = CAL_REAL[i + 1];

    if (x1 == x0)
        return raw;

    return y0 + (raw - x0) * (y1 - y0) / (x1 - x0);
}

TaskHandle_t sensorTaskHandle = nullptr;

volatile uint32_t echoRiseUs = 0;
volatile uint32_t echoFallUs = 0;
volatile bool echoRose = false;

// state filter (hanya disentuh oleh sensorTask)
float sBuf[SAMPLE];
int sIdx = 0;
int sCount = 0;
int sMiss = 0;
float sFiltered = -1;

void IRAM_ATTR echoISR()
{
    uint32_t now = (uint32_t)esp_timer_get_time();

    if (gpio_get_level((gpio_num_t)ECHO_PIN))
    {
        echoRiseUs = now;
        echoRose = true;
    }
    else if (echoRose)
    {
        echoFallUs = now;
        echoRose = false;

        BaseType_t hpWoken = pdFALSE;
        vTaskNotifyGiveFromISR(sensorTaskHandle, &hpWoken);

        if (hpWoken)
            portYIELD_FROM_ISR();
    }
}

// Trimmed mean: urutkan, buang nilai ekstrem, rata-rata sisanya
float robustEstimate()
{
    float t[SAMPLE];

    for (int i = 0; i < sCount; i++)
        t[i] = sBuf[i];

    for (int i = 1; i < sCount; i++)
    {
        float key = t[i];
        int j = i - 1;

        while (j >= 0 && t[j] > key)
        {
            t[j + 1] = t[j];
            j--;
        }

        t[j + 1] = key;
    }

    if (sCount >= 5)
    {
        int trim = sCount / 4; // buang 25% terkecil & terbesar, rata-rata sisanya
        float sum = 0;

        for (int i = trim; i < sCount - trim; i++)
            sum += t[i];

        return sum / (sCount - 2 * trim);
    }

    return t[sCount / 2]; // sampel masih sedikit: median
}

void addSample(float cm)
{
    sMiss = 0;

    sBuf[sIdx] = cm;
    sIdx = (sIdx + 1) % SAMPLE;

    if (sCount < SAMPLE)
        sCount++;

    if (sCount < 3)
        return;

    // Deteksi gerakan cepat: 3 sampel terakhir semuanya jauh (>1.5 cm) di sisi yang sama
    // dan saling konsisten -> buang riwayat lama, langsung ikuti posisi baru
    if (sFiltered >= 0)
    {
        float a = sBuf[(sIdx + SAMPLE - 1) % SAMPLE];
        float b = sBuf[(sIdx + SAMPLE - 2) % SAMPLE];
        float c = sBuf[(sIdx + SAMPLE - 3) % SAMPLE];
        float da = a - sFiltered, db = b - sFiltered, dc = c - sFiltered;

        bool up = (da > 1.5f && db > 1.5f && dc > 1.5f);
        bool dn = (da < -1.5f && db < -1.5f && dc < -1.5f);

        if ((up || dn) && fabsf(a - c) < 1.5f)
        {
            sBuf[0] = c;
            sBuf[1] = b;
            sBuf[2] = a;
            sIdx = 3;
            sCount = 3;
            sFiltered = (a + b + c) / 3.0f;
            gDistance = sFiltered;
            return;
        }
    }

    float est = robustEstimate();

    if (sFiltered < 0)
    {
        sFiltered = est;
    }
    else
    {
        float diff = fabsf(est - sFiltered);

        if (diff >= 0.1f) // di bawah 0.1 cm dianggap getaran, tampilan tetap diam
        {
            // makin besar perubahan, makin cepat mengikuti (>3 cm: langsung loncat)
            float alpha = (diff > 3) ? 1.0f : ((diff > 1) ? 0.6f : 0.25f);
            sFiltered += alpha * (est - sFiltered);
        }
    }

    gDistance = sFiltered;
}

void sampleMiss()
{
    if (sMiss < 100)
        sMiss++;

    if (sMiss >= 6) // gagal 6x beruntun (~240 ms) = tidak terdeteksi
    {
        sFiltered = -1;
        gDistance = -1;
        sCount = 0;
        sIdx = 0;
    }
}

void sensorTask(void *)
{
    attachInterrupt(digitalPinToInterrupt(ECHO_PIN), echoISR, CHANGE);

    TickType_t lastWake = xTaskGetTickCount();

    for (;;)
    {
        echoRose = false;
        ulTaskNotifyTake(pdTRUE, 0); // buang notifikasi sisa

        digitalWrite(TRIG_PIN, LOW);
        delayMicroseconds(4);
        digitalWrite(TRIG_PIN, HIGH);
        delayMicroseconds(10);
        digitalWrite(TRIG_PIN, LOW);

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ECHO_TIMEOUT_MS)) > 0)
        {
            uint32_t duration = echoFallUs - echoRiseUs; // mikrodetik
            float raw = duration * CM_PER_US / 2.0f;

            if (raw >= MIN_CM && raw <= MAX_CM)
            {
                addSample(calibrate(raw));

                if (CAL_DEBUG)
                {
                    static float rawAvg = -1;
                    static uint32_t lastPrint = 0;

                    rawAvg = (rawAvg < 0) ? raw : rawAvg + 0.1f * (raw - rawAvg);

                    if (millis() - lastPrint >= 500)
                    {
                        lastPrint = millis();
                        Serial.printf("RAW=%.2f cm  REAL=%.2f cm\n", rawAvg, (float)gDistance);
                    }
                }
            }
            else
                sampleMiss();
        }
        else
        {
            sampleMiss();
        }

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(PING_PERIOD_MS));
    }
}

/*************** STATUS JARAK ****************/
// Level: 0 BAHAYA (<10) | 1 WASPADA (10..30) | 2 AMAN (>30) | -1 tidak terdeteksi
// Masuk ke level yang lebih berbahaya langsung sesuai batas;
// keluar dari level itu butuh lewat batas +0.5 cm (cegah badge berkedip).

int distanceLevel(float d)
{
    static int lvl = -1;
    const float H = 0.5f;

    if (d < 0)
    {
        lvl = -1;
        return -1;
    }

    if (lvl < 0)
    {
        lvl = (d < 10) ? 0 : ((d <= 30) ? 1 : 2);
        return lvl;
    }

    if (lvl == 0)
    {
        if (d >= 10 + H)
            lvl = (d > 30 + H) ? 2 : 1;
    }
    else if (lvl == 1)
    {
        if (d < 10)
            lvl = 0;
        else if (d > 30 + H)
            lvl = 2;
    }
    else
    {
        if (d < 10)
            lvl = 0;
        else if (d <= 30)
            lvl = 1;
    }

    return lvl;
}

/*************** UTIL ****************/

String jsonEscape(const String &s)
{
    String o;
    o.reserve(s.length() + 8);

    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s[i];

        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += c;
        }
        else if ((uint8_t)c >= 0x20)
        {
            o += c;
        }
    }

    return o;
}

// true jika request datang lewat interface Access Point (HP tersambung ke ESP32-CONFIG-...)
bool fromAP()
{
    return server.client().localIP() == WiFi.softAPIP();
}

// Dashboard hanya boleh dibuka jika ESP32 sudah terhubung WiFi (punya IP STA)
bool dashboardOpen()
{
    return staState == STA_ONLINE;
}

const char *staStateName()
{
    switch (staState)
    {
    case STA_CONNECTING:
        return "connecting";
    case STA_ONLINE:
        return "online";
    case STA_LOST:
        return "lost";
    case STA_FAILED:
        return "failed";
    default:
        return "idle";
    }
}

/*************** HALAMAN SETUP (CAPTIVE PORTAL) ***************/

const char SETUP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 WiFi Setup</title>
<style>
html,body{overscroll-behavior-y:contain}
body{font-family:Arial,sans-serif;background:#f2f2f2;margin:0;padding:16px}
.card{background:#fff;max-width:420px;margin:0 auto;padding:22px;border-radius:15px;box-shadow:0 0 10px #aaa}
h2{text-align:center;margin-top:0}
.st{background:#eef3ff;border-radius:10px;padding:8px 12px;font-size:14px;margin-bottom:14px}
ul{list-style:none;padding:0;margin:8px 0;max-height:210px;overflow:auto;overscroll-behavior:contain;-webkit-overflow-scrolling:touch;border:1px solid #ddd;border-radius:10px}
li{padding:10px 12px;border-bottom:1px solid #eee;display:flex;justify-content:space-between;cursor:pointer}
li:last-child{border-bottom:0}
li.sel{background:#dce8ff}
label{font-size:14px;color:#444}
input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0 12px;font-size:16px;border:1px solid #bbb;border-radius:8px}
button{background:#0066ff;color:#fff;border:0;padding:12px 20px;border-radius:10px;font-size:16px;width:100%}
button.sec{background:#777;width:auto;padding:6px 12px;font-size:13px;float:right}
#info{font-size:13px;color:#666}
a{color:#0066ff}
.lock{color:#999;font-size:14px}
button.warn{background:#c62828;margin-top:10px}
</style>
</head>
<body>
<div class="card">
<h2>ESP32 WiFi Provisioning</h2>
<div class="st" id="st">Memeriksa status...</div>
<b>Daftar WiFi terdeteksi</b>
<button class="sec" type="button" onclick="loadScan(true)">Pindai ulang</button>
<div id="info">Memindai WiFi...</div>
<ul id="list"></ul>
<form id="f">
<label>Nama WiFi (SSID)</label>
<input id="ssid" name="ssid" maxlength="32" placeholder="Pilih dari daftar atau ketik manual" required>
<label>Kata Sandi (Password)</label>
<input id="pw" name="password" type="password" maxlength="63" placeholder="Kosongkan jika WiFi terbuka">
<button type="submit">Simpan &amp; Sambungkan</button>
</form>
<button id="fg" class="warn" type="button" style="display:none" onclick="forget()">Putuskan &amp; Hapus WiFi Tersimpan</button>
<p id="msg"></p>
<p style="text-align:center" id="dl"></p>
</div>
<script>
const $=i=>document.getElementById(i);
let selSSID='',lastSeq=-1,busy=false,count=0,scanning=false;

function updateInfo(){
  if(count==0)$('info').textContent=scanning?'Memindai WiFi...':'Tidak ada WiFi ditemukan.';
  else $('info').textContent=count+' jaringan ditemukan - ketuk untuk memilih'+(scanning?' (memindai...)':'');
}

function render(l){
  const u=$('list'),top=u.scrollTop;
  u.innerHTML='';
  count=l.length;
  l.forEach(n=>{
    const li=document.createElement('li');
    const a=document.createElement('span');a.textContent=n.ssid;
    const b=document.createElement('span');b.textContent=n.rssi+' dBm';b.style.color='#666';
    li.append(a,b);
    if(n.ssid===selSSID)li.classList.add('sel');
    li.onclick=()=>{
      selSSID=n.ssid;
      $('ssid').value=n.ssid;
      [...u.children].forEach(x=>x.classList.remove('sel'));
      li.classList.add('sel');
      $('pw').focus();
    };
    u.append(li);
  });
  u.scrollTop=top;
}

async function loadScan(r){
  if(busy)return;
  busy=true;
  try{
    const j=await (await fetch('/scan'+(r?'?rescan=1':''),{cache:'no-store'})).json();
    scanning=j.scanning;
    if(j.seq!==lastSeq){lastSeq=j.seq;render(j.list);}
    updateInfo();
  }catch(e){}
  busy=false;
}

function showStatus(s){
  const m={
    idle:'STA: belum ada WiFi tersimpan',
    connecting:'STA: menyambung ke "'+s.ssid+'"...',
    online:'STA: terhubung ke "'+s.ssid+'" - IP '+s.ip,
    lost:'STA: terputus dari "'+s.ssid+'"',
    failed:'STA: gagal menyambung ke "'+s.ssid+'"'
  };
  $('st').textContent=(m[s.state]||s.state)+'  |  AP: '+s.ap_ip;
  $('fg').style.display=s.ssid?'block':'none';
  $('dl').innerHTML=(s.state=='online')
    ?'<a href="/dashboard">Buka Dashboard Jarak</a>'
    :'<span class="lock">&#128274; Dashboard terkunci - sambungkan WiFi dulu</span>';
}

let tries=0;
async function poll(){
  tries++;
  try{
    const s=await (await fetch('/wifistatus',{cache:'no-store'})).json();
    showStatus(s);
    if(s.state=='online'){
      $('msg').textContent='Berhasil! IP STA: '+s.ip+'. Sambungkan HP ke WiFi yang sama, lalu buka http://'+s.ip+'/';
      return;
    }
    if(s.state=='failed'){
      $('msg').textContent='Gagal terhubung. Periksa SSID/password lalu coba lagi.';
      return;
    }
    $('msg').textContent='Menyambung...';
  }catch(e){
    $('msg').textContent='Menunggu ESP32... (jika HP terputus, sambungkan ulang ke WiFi ESP32-CONFIG)';
  }
  if(tries<90)setTimeout(poll,1000);
}

$('f').addEventListener('submit',async e=>{
  e.preventDefault();
  const ssid=$('ssid').value.trim();
  if(!ssid){$('msg').textContent='SSID wajib diisi';return;}
  $('msg').textContent='Menyimpan...';
  try{
    const r=await fetch('/save',{method:'POST',body:new URLSearchParams({ssid:ssid,password:$('pw').value})});
    const j=await r.json();
    if(!j.ok){$('msg').textContent=j.error||'Gagal menyimpan';return;}
  }catch(err){$('msg').textContent='Gagal mengirim data ke ESP32';return;}
  tries=0;poll();
});

async function forget(){
  if(!confirm('Putuskan WiFi dan hapus WiFi/hotspot yang tersimpan? Dashboard akan terkunci.'))return;
  $('msg').textContent='Memutuskan...';
  try{
    const j=await (await fetch('/forget',{method:'POST'})).json();
    $('msg').textContent=j.ok?'WiFi diputuskan dan data WiFi dihapus.':'Gagal memutuskan WiFi';
  }catch(e){$('msg').textContent='Gagal menghubungi ESP32';}
  tries=999;           // hentikan polling status penyambungan
  refreshStatus();
}

async function refreshStatus(){
  try{showStatus(await (await fetch('/wifistatus',{cache:'no-store'})).json());}catch(e){}
}

refreshStatus();
loadScan(false);
setInterval(()=>loadScan(false),2000);   // ambil hasil scan terbaru (ESP scan tiap 15 dtk)
setInterval(refreshStatus,3000);
</script>
</body>
</html>
)rawliteral";

/*************** HALAMAN DASHBOARD ***************/

const char DASH_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Distance Monitor</title>
<style>
html,body{overscroll-behavior-y:contain}
body{font-family:Arial,sans-serif;background:#eee;margin:0;padding:12px;text-align:center}
.card{background:#fff;max-width:460px;margin:16px auto;padding:22px;border-radius:20px;box-shadow:0 0 10px #aaa}
h2{margin:0 0 6px}
.sub{font-size:13px;color:#555;margin:4px 0}
.dist{font-size:56px;font-weight:bold;margin:6px 0}
.badge{display:inline-block;color:#fff;font-size:20px;font-weight:bold;padding:10px 18px;border-radius:12px;background:#757575}
.bar{height:18px;background:#ddd;border-radius:9px;overflow:hidden;margin:18px 0 4px}
#fill{height:100%;width:0;background:#757575;transition:width .08s linear}
button{background:#c62828;color:#fff;border:0;padding:10px 18px;border-radius:10px;font-size:15px;margin-top:6px}
a{color:#0066ff}
</style>
</head>
<body>
<div class="card">
<h2>SMART DISTANCE MONITOR</h2>
<div class="sub" id="dev">Device: - | IP: -</div>
<div class="sub" id="conn">Status koneksi: -</div>
<p>Jarak Objek</p>
<div id="dist" class="dist">-- cm</div>
<div id="status" class="badge">-</div>
<div class="bar"><div id="fill"></div></div>
<div class="sub">Skala bar: 0 - 100 cm</div>
<div class="sub">Pembaruan terakhir: <span id="ts">-</span></div>
<p><a href="/setup">Pengaturan WiFi</a></p>
<button id="rst">Reset Pengaturan Wi-Fi</button>
<button id="fg">Putuskan &amp; Hapus WiFi</button>
</div>
<script>
const $=i=>document.getElementById(i);
const C={red:'#d32f2f',orange:'#f57c00',green:'#2e7d32',gray:'#757575'};

async function getJSON(u,ms){
  const c=new AbortController();
  const t=setTimeout(()=>c.abort(),ms);
  try{const r=await fetch(u,{cache:'no-store',signal:c.signal});return await r.json();}
  finally{clearTimeout(t);}
}

async function tick(){
  try{
    const d=await getJSON('/data',1500);
    if(d.locked){location.reload();return;}   // koneksi WiFi ESP32 hilang -> halaman terkunci
    const col=C[d.color]||C.gray;
    $('dist').textContent=d.distance<0?'-- cm':d.distance.toFixed(1)+' cm';
    $('dist').style.color=col;
    $('status').textContent=d.status;
    $('status').style.background=col;
    $('fill').style.width=(d.distance<0?0:Math.min(100,d.distance))+'%';
    $('fill').style.background=col;
    $('ts').textContent=new Date().toLocaleTimeString('id-ID')+' WIB';
  }catch(e){
    $('status').textContent='KONEKSI TERPUTUS';
    $('status').style.background=C.gray;
  }
  setTimeout(tick,40);
}

async function wifi(){
  try{
    const s=await getJSON('/wifistatus',2500);
    if(s.state!='online'){location.reload();return;}
    $('dev').textContent='Device: '+s.device+' | IP: '+location.hostname;
    $('conn').textContent='Status Koneksi: TERHUBUNG KE WI-FI (ONLINE) - '+s.ssid;
  }catch(e){}
  setTimeout(wifi,3000);
}

$('rst').onclick=async()=>{
  if(!confirm('Hapus pengaturan WiFi dan restart ESP32?'))return;
  try{await fetch('/reset',{method:'POST'});}catch(e){}
  $('status').textContent='Restart...';
};

$('fg').onclick=async()=>{
  if(!confirm('Putuskan WiFi dan hapus WiFi/hotspot tersimpan?\nDashboard akan terkunci dan koneksi ini terputus. Sambungkan HP ke WiFi ESP32-CONFIG untuk setup ulang.'))return;
  try{await fetch('/forget',{method:'POST'});}catch(e){}
  $('status').textContent='WiFi diputuskan';
};

tick();wifi();
</script>
</body>
</html>
)rawliteral";

/*************** HALAMAN DASHBOARD TERKUNCI ***************/

const char LOCK_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Dashboard Terkunci</title>
<style>
html,body{overscroll-behavior-y:contain}
body{font-family:Arial,sans-serif;background:#eee;margin:0;padding:12px;text-align:center}
.card{background:#fff;max-width:420px;margin:40px auto;padding:28px 22px;border-radius:20px;box-shadow:0 0 10px #aaa}
.ic{font-size:56px}
h2{margin:8px 0}
p{color:#555;font-size:14px}
a{color:#0066ff}
</style>
</head>
<body>
<div class="card">
<div class="ic">&#128274;</div>
<h2>Dashboard Terkunci</h2>
<p>ESP32 belum terhubung ke WiFi (belum ada IP STA).<br>Sambungkan ESP32 ke WiFi/hotspot lewat halaman pengaturan terlebih dahulu.</p>
<p id="s">Memeriksa status...</p>
<p><a href="/setup">Buka Pengaturan WiFi</a></p>
</div>
<script>
async function chk(){
  try{
    const s=await (await fetch('/wifistatus',{cache:'no-store'})).json();
    if(s.state=='online'){location.replace('/dashboard');return;}
    document.getElementById('s').textContent='Status WiFi: '+s.state;
  }catch(e){}
  setTimeout(chk,2000);
}
chk();
</script>
</body>
</html>
)rawliteral";

/*************** SCAN WIFI (async + cache) ***************/

bool scanAllowed()
{
    // Scan memindahkan radio antar-channel; jangan ganggu proses menyambung
    return staState != STA_CONNECTING;
}

void startScan()
{
    if (scanActive || !scanAllowed())
        return;

    WiFi.scanDelete();
    WiFi.scanNetworks(true); // async
    scanActive = true;
}

// Ubah hasil scan jadi array JSON (tanpa SSID kosong & duplikat, urut RSSI menurun)
void buildScanCache(int n)
{
    int idx[32];
    int cnt = 0;

    for (int i = 0; i < n && cnt < 32; i++)
    {
        String s = WiFi.SSID(i);

        if (s.length() == 0)
            continue;

        bool best = true;

        for (int k = 0; k < n; k++)
        {
            if (k == i || WiFi.SSID(k) != s)
                continue;

            if (WiFi.RSSI(k) > WiFi.RSSI(i) || (WiFi.RSSI(k) == WiFi.RSSI(i) && k < i))
            {
                best = false;
                break;
            }
        }

        if (best)
            idx[cnt++] = i;
    }

    for (int i = 1; i < cnt; i++)
    {
        int key = idx[i];
        int j = i - 1;

        while (j >= 0 && WiFi.RSSI(idx[j]) < WiFi.RSSI(key))
        {
            idx[j + 1] = idx[j];
            j--;
        }

        idx[j + 1] = key;
    }

    String out = "[";

    for (int i = 0; i < cnt; i++)
    {
        if (i)
            out += ",";

        out += "{\"ssid\":\"";
        out += jsonEscape(WiFi.SSID(idx[i]));
        out += "\",\"rssi\":";
        out += WiFi.RSSI(idx[i]);
        out += "}";
    }

    out += "]";

    scanList = out;
    scanSeq++;
}

void scanTask()
{
    // 1) ambil hasil scan yang sudah selesai
    if (scanActive)
    {
        int n = WiFi.scanComplete();

        if (n == WIFI_SCAN_RUNNING)
            return;

        if (n >= 0)
            buildScanCache(n);

        WiFi.scanDelete();
        scanActive = false;
    }

    // 2) scan otomatis tiap 15 detik.
    //    Saat sudah ONLINE, scan hanya jalan kalau halaman setup sedang dibuka
    //    (scan membuat radio pindah channel sesaat -> bisa mengganggu dashboard).
    bool wanted = (staState != STA_ONLINE) || (millis() - lastScanReq < 20000);

    if (wanted && (lastAutoScan == 0 || millis() - lastAutoScan >= AUTO_SCAN_MS))
    {
        lastAutoScan = millis();
        if (lastAutoScan == 0)
            lastAutoScan = 1;

        startScan();
    }
}

/*************** HANDLER WEB ***************/

void handleSetup()
{
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", SETUP_PAGE);
}

void handleDashboard()
{
    server.sendHeader("Cache-Control", "no-store");

    if (!dashboardOpen()) // terkunci selama belum ada IP STA
    {
        server.send_P(403, "text/html", LOCK_PAGE);
        return;
    }

    server.send_P(200, "text/html", DASH_PAGE);
}

// "/" : HP yang tersambung ke AP (192.168.4.1) -> halaman setup WiFi
//       HP yang membuka lewat IP STA            -> dashboard
void handleRoot()
{
    if (fromAP())
        handleSetup();
    else
        handleDashboard();
}

void handleNotFound()
{
    if (fromAP()) // captive portal: semua alamat lain diarahkan ke ESP32
    {
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
    }
    else
    {
        server.send(404, "text/plain", "Not found");
    }
}

void handleSensorData()
{
    server.sendHeader("Cache-Control", "no-store");

    if (!dashboardOpen())
    {
        server.send(403, "application/json", "{\"locked\":true}");
        return;
    }

    float d = gDistance;
    int lv = distanceLevel(d);

    const char *status;
    const char *color;

    switch (lv)
    {
    case 0:
        status = "BAHAYA / OBJEK TERLALU DEKAT";
        color = "red";
        break;
    case 1:
        status = "PERINGATAN / WASPADA";
        color = "orange";
        break;
    case 2:
        status = "AMAN / BEBAS RINTANGAN";
        color = "green";
        break;
    default:
        status = "TIDAK TERDETEKSI";
        color = "gray";
        break;
    }

    char out[180];
    snprintf(out, sizeof(out),
             "{\"distance\":%.1f,\"level\":%d,\"status\":\"%s\",\"color\":\"%s\"}",
             d, lv, status, color);

    server.send(200, "application/json", out);
}

void handleWifiStatus()
{
    String j = "{\"device\":\"";
    j += AP_SSID;
    j += "\",\"state\":\"";
    j += staStateName();
    j += "\",\"ssid\":\"";
    j += jsonEscape(wifiSSID);
    j += "\",\"ip\":\"";
    j += (staState == STA_ONLINE) ? WiFi.localIP().toString() : String("");
    j += "\",\"rssi\":";
    j += (staState == STA_ONLINE) ? WiFi.RSSI() : 0;
    j += ",\"ap_ip\":\"";
    j += WiFi.softAPIP().toString();
    j += "\",\"clients\":";
    j += WiFi.softAPgetStationNum();
    j += "}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
}

// Mengembalikan hasil scan terakhir (cache). Scan sendiri berjalan otomatis
// tiap 15 detik di scanTask(); "rescan=1" memaksa scan segera.
void handleScan()
{
    lastScanReq = millis();
    if (lastScanReq == 0)
        lastScanReq = 1;

    if (server.hasArg("rescan"))
        startScan();
    else if (scanSeq == 0 && !scanActive)
        startScan(); // belum pernah scan sama sekali

    String out;
    out.reserve(scanList.length() + 48);
    out = "{\"scanning\":";
    out += scanActive ? "true" : "false";
    out += ",\"seq\":";
    out += scanSeq;
    out += ",\"list\":";
    out += scanList;
    out += "}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

/*************** WIFI STATION ***************/

void loadWiFiConfig()
{
    preferences.begin("wifi", true);
    wifiSSID = preferences.getString("ssid", "");
    wifiPASS = preferences.getString("pass", "");
    preferences.end();

    Serial.println();
    Serial.print("SSID tersimpan : ");
    Serial.println(wifiSSID == "" ? "(kosong)" : wifiSSID);
}

// Non-blocking: hanya memulai koneksi, hasilnya dipantau staTask()
void startStationConnect()
{
    if (wifiSSID == "")
    {
        staState = STA_IDLE;
        return;
    }

    // hentikan scan yang sedang jalan supaya tidak mengganggu proses menyambung
    if (scanActive)
    {
        WiFi.scanDelete();
        scanActive = false;
    }

    WiFi.disconnect(); // AP tetap hidup
    WiFi.begin(wifiSSID.c_str(), wifiPASS.c_str());

    staState = STA_CONNECTING;
    staConnectStart = millis();

    Serial.print("Menghubungkan WiFi: ");
    Serial.println(wifiSSID);
}

void onWifiOnline()
{
    Serial.println();
    Serial.println("=== WIFI TERHUBUNG (STA) ===");
    Serial.print("SSID    : ");
    Serial.println(wifiSSID);
    Serial.print("IP STA  : ");
    Serial.println(WiFi.localIP());

    notifUntil = millis() + 5000; // notifikasi + IP STA di LCD
    if (notifUntil == 0)
        notifUntil = 1;
}

void staTask()
{
    static unsigned long lastRetry = 0;

    // wifiSSID kosong = WiFi baru dihapus; jangan dianggap online walau status belum update
    bool connected = (WiFi.status() == WL_CONNECTED) && wifiSSID != "";

    if (connected)
    {
        if (staState != STA_ONLINE)
        {
            staState = STA_ONLINE;
            onWifiOnline();
        }
        return;
    }

    if (staState == STA_ONLINE)
    {
        staState = STA_LOST; // dashboard otomatis terkunci lagi
        lastRetry = millis();
        Serial.println("WiFi terputus");
    }
    else if (staState == STA_CONNECTING && millis() - staConnectStart > 20000)
    {
        staState = STA_FAILED;
        lastRetry = millis();
        Serial.println("Gagal koneksi WiFi");
    }
    else if ((staState == STA_FAILED || staState == STA_LOST) &&
             wifiSSID != "" &&
             millis() - lastRetry > 20000 &&
             !scanActive)
    {
        lastRetry = millis();
        startStationConnect();
    }
}

void handleSave()
{
    String ssid = server.arg("ssid");
    String pass = server.arg("password");

    ssid.trim();

    if (ssid.length() == 0 || ssid.length() > 32)
    {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"SSID tidak valid\"}");
        return;
    }

    if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8))
    {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"Password harus 8-63 karakter (atau kosong)\"}");
        return;
    }

    wifiSSID = ssid;
    wifiPASS = pass;

    preferences.begin("wifi", false);
    preferences.putString("ssid", wifiSSID);
    preferences.putString("pass", wifiPASS);
    preferences.end();

    server.send(200, "application/json", "{\"ok\":true}");

    startStationConnect();
}

/*************** RESET KONFIGURASI ***************/

void resetConfig()
{
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();

    wifiSSID = "";
    wifiPASS = "";

    Serial.println("Konfigurasi WiFi dihapus, restart...");

    lcdLine(0, "Reset WiFi...");
    lcdLine(1, "Restart");

    restartAt = millis() + 1200;
    if (restartAt == 0)
        restartAt = 1;
}

// Putuskan WiFi/hotspot & hapus kredensial tersimpan (tanpa restart, AP tetap aktif)
void forgetWifi()
{
    preferences.begin("wifi", false);
    preferences.clear();
    preferences.end();

    wifiSSID = "";
    wifiPASS = "";

    if (scanActive)
    {
        WiFi.scanDelete();
        scanActive = false;
    }

    WiFi.disconnect(false, false); // putuskan STA, AP tetap hidup

    staState = STA_IDLE; // dashboard otomatis terkunci
    notifUntil = 0;
    lastAutoScan = 0;    // scan WiFi segera dijalankan lagi

    Serial.println("WiFi diputuskan & konfigurasi dihapus (AP tetap aktif)");
}

void handleForget()
{
    if (!dashboardOpen() && !fromAP())
    {
        server.send(403, "application/json", "{\"ok\":false}");
        return;
    }

    server.send(200, "application/json", "{\"ok\":true}");
    forgetWifi();
}

void handleReset()
{
    if (!dashboardOpen() && !fromAP()) // dari jaringan STA hanya saat online
    {
        server.send(403, "application/json", "{\"ok\":false}");
        return;
    }

    server.send(200, "application/json", "{\"ok\":true}");
    resetConfig();
}

// Tahan tombol BOOT 3 detik (setelah ESP selesai boot)
void resetButtonTask()
{
    static unsigned long pressStart = 0;

    if (restartAt)
        return;

    if (digitalRead(RESET_BTN_PIN) == LOW)
    {
        if (pressStart == 0)
            pressStart = millis();
        else if (millis() - pressStart >= 3000)
            resetConfig();
    }
    else
    {
        pressStart = 0;
    }
}

/*************** LED ***************/
// ONLINE = menyala konstan, menyambung = berkedip cepat, lainnya = mati

void ledTask()
{
    if (staState == STA_ONLINE)
        digitalWrite(LED_PIN, HIGH);
    else if (staState == STA_CONNECTING)
        digitalWrite(LED_PIN, (millis() / 250) % 2);
    else
        digitalWrite(LED_PIN, LOW);
}

/*************** ACCESS POINT + CAPTIVE PORTAL ***************/

void startAccessPoint()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA); // AP selalu aktif, STA menyusul
    WiFi.setSleep(false);   // matikan power-save WiFi: latensi lebih rendah

    WiFi.softAP(AP_SSID);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP()); // captive portal

    Serial.println();
    Serial.println("=== ACCESS POINT MODE ===");
    Serial.print("SSID AP : ");
    Serial.println(AP_SSID);
    Serial.print("IP AP   : ");
    Serial.println(WiFi.softAPIP());
}

/******************************************************
 SETUP
 ******************************************************/

void setup()
{
    Serial.begin(115200);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(RESET_BTN_PIN, INPUT_PULLUP);

    digitalWrite(TRIG_PIN, LOW);
    digitalWrite(LED_PIN, LOW);

    lcdBegin();
    loadWiFiConfig();
    startAccessPoint();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/setup", HTTP_GET, handleSetup);
    server.on("/dashboard", HTTP_GET, handleDashboard);
    server.on("/data", HTTP_GET, handleSensorData);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/wifistatus", HTTP_GET, handleWifiStatus);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/forget", HTTP_POST, handleForget);
    server.onNotFound(handleNotFound);
    server.begin();

    Serial.println("Web Server Aktif");

    // Sensor: task sendiri di core 1, prioritas lebih tinggi dari loop()
    xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 3, &sensorTaskHandle, 1);

    // Baru setelah AP & server aktif, coba konek WiFi tersimpan (non-blocking)
    startStationConnect();
}

/******************************************************
 LOOP
 ******************************************************/

void loop()
{
    server.handleClient();
    dnsServer.processNextRequest();

    staTask();
    scanTask(); // scan WiFi otomatis tiap 15 detik + cache hasil
    lcdTask();
    resetButtonTask();
    ledTask();

    if (restartAt && (long)(millis() - restartAt) >= 0)
        ESP.restart();

    delay(1);
}
