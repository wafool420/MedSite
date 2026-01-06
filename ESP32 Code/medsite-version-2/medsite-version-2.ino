#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "esp_wifi.h"
#include <Preferences.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <Wire.h>
#include <MAX30105.h>
#include <math.h>

// ---------------------------------------
// OPTIONAL: uncomment this to always clear
// saved WiFi on boot while debugging.
// ---------------------------------------
// #define RESET_SAVED_WIFI_ON_BOOT
// ---------------------------------------

// ===================== DEFAULT DEVICE CONFIG (overridable via portal/NVS) =====================
String cfgIngestUrl    = "https://medsite.onrender.com/api/ingest/";
String cfgDeviceId     = "c3-001";
String cfgPatientCode  = "";              // X-PUBLIC-CODE
String cfgDeviceName   = "MedSite C3";
// =============================================================================================

// ---------- Captive Portal ----------
WebServer   server(80);
DNSServer   dns;
Preferences prefs;

const byte DNS_PORT = 53;

// AP used for config mode
const char* AP_SSID     = "C3-Config";
// const char* AP_PASSWORD = nullptr;
IPAddress apIP(192, 168, 4, 1);

String networksHTML;
bool   inConfigMode = false;

// ---------- MAX30105 / I2C ----------
#define SDA_PIN 5
#define SCL_PIN 6

// Finger hysteresis
#define FINGER_ON_THRESHOLD  9000
#define FINGER_OFF_THRESHOLD 7000

#define MIN_BPM 40
#define MAX_BPM 200

MAX30105 sensor;

// Raw signals
long  currentIr   = 0;
long  currentRed  = 0;

// Derived values
int   currentBpm      = 0;
float currentSpO2     = 0.0f;
float currentPI       = 0.0f;
float currentRR       = 0.0f;
int   currentSBP      = 0;
int   currentDBP      = 0;
float currentTemp     = 0.0f;

bool  hasFinger       = false;
bool  hasValidBpm     = false;
bool  hasValidSpO2    = false;
bool  hasValidTemp    = false;

bool sensorReady = false;

// ---- Beat averaging ----
const int BEAT_HISTORY = 5;
float beatBpmHistory[BEAT_HISTORY];
int   beatIndex = 0;
int   beatCount = 0;
static float posAbsEma = 0.0f;
static float negAbsEma = 0.0f;
static bool  useNegPolarity = false;   // true = detect downward pulses

// ---- SpO2 / PI window ----
const int WINDOW_SIZE = 50;
float irWindow[WINDOW_SIZE];
float redWindow[WINDOW_SIZE];
int   windowIndex = 0;
int   windowCount = 0;

// ======= TLS client =======
WiFiClientSecure tlsClient;

// ===================== POSTING =====================
static const uint32_t POST_INTERVAL_MS = 1000;
static const uint32_t HTTP_TIMEOUT_MS  = 1500;

// ===================== SNAPSHOT FOR POST TASK =====================
typedef struct {
  long  ir;
  long  red;
  bool  finger;

  bool  validBpm;
  int   bpm;

  bool  validSpO2;
  float spo2;

  float pi;

  bool  validTemp;
  float temp;

  float rr;
  int   sbp;
  int   dbp;
} VitalsSnapshot;

static VitalsSnapshot gSnap;
static portMUX_TYPE   gSnapMux = portMUX_INITIALIZER_UNLOCKED;

// ===================== SENSOR AUTO-GAIN =====================
// Start moderate (prevents 262143 clipping)
static uint8_t irAmp  = 0x30;
static uint8_t redAmp = 0x24;

static uint32_t lastGainAdjust = 0;

static void applyLedAmps() {
  sensor.setPulseAmplitudeIR(irAmp);
  sensor.setPulseAmplitudeRed(redAmp);
}

static void autoGain(long ir) {
  // only adjust once/sec
  if (millis() - lastGainAdjust < 1000) return;
  lastGainAdjust = millis();

  // If clipping/saturated -> reduce LED currents
  if (ir >= 240000) {
    if (irAmp > 0x08) irAmp -= 0x08;
    if (redAmp > 0x06) redAmp -= 0x06;
    applyLedAmps();
    Serial.print("[GAIN] too high -> IRamp=0x"); Serial.print(irAmp, HEX);
    Serial.print(" REDamp=0x"); Serial.println(redAmp, HEX);
    return;
  }

  // If too low -> increase LED currents
  if (ir <= 15000) {
    if (irAmp < 0x7F) irAmp += 0x08;
    if (redAmp < 0x5F) redAmp += 0x06;
    applyLedAmps();
    Serial.print("[GAIN] too low -> IRamp=0x"); Serial.print(irAmp, HEX);
    Serial.print(" REDamp=0x"); Serial.println(redAmp, HEX);
    return;
  }

  // In the “good zone” (roughly 20k–200k) do nothing
}

// ===================== NVS HELPERS =====================
void clearSavedWifi() {
  Serial.println("Clearing saved WiFi credentials from Preferences...");
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
}

bool loadSavedWifi(String &ssid, String &pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();

  ssid.trim();
  pass.trim();

  if (ssid.length() == 0) {
    Serial.println("No saved WiFi credentials in NVS.");
    return false;
  }

  Serial.println("Loaded WiFi credentials from NVS:");
  Serial.print("  SSID: '"); Serial.print(ssid); Serial.println("'");
  Serial.print("  PASS length: "); Serial.println(pass.length());
  return true;
}

void saveWifi(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.println("WiFi credentials saved to NVS.");
}

void clearDeviceConfig() {
  Serial.println("Clearing device config from Preferences...");
  prefs.begin("dev", false);
  prefs.clear();
  prefs.end();
}

void loadDeviceConfig() {
  prefs.begin("dev", true);

  String url  = prefs.getString("url", "");
  String id   = prefs.getString("id", "");
  String name = prefs.getString("name", "");
  String pub  = prefs.getString("pub", "");
  String pair = prefs.getString("pair", "");

  prefs.end();

  url.trim(); id.trim(); name.trim(); pub.trim(); pair.trim();

  if (url.length())  cfgIngestUrl  = url;
  if (id.length())   cfgDeviceId   = id;
  if (name.length()) cfgDeviceName = name;

  if (pub.length()) cfgPatientCode = pub;
  else if (pair.length()) cfgPatientCode = pair;

  Serial.println("Loaded device config:");
  Serial.println("  url=" + cfgIngestUrl);
  Serial.println("  id=" + cfgDeviceId);
  Serial.println("  patient_code=" + cfgPatientCode);
  Serial.println("  name=" + cfgDeviceName);
}

void saveDeviceConfig() {
  prefs.begin("dev", false);
  prefs.putString("url",  cfgIngestUrl);
  prefs.putString("id",   cfgDeviceId);
  prefs.putString("pub",  cfgPatientCode);
  prefs.putString("name", cfgDeviceName);
  prefs.remove("pair");
  prefs.end();
  Serial.println("Device config saved to NVS.");
}

// ===================== NETWORK SCAN =====================
void buildNetworksList() {
  Serial.println("Scanning for WiFi networks...");
  networksHTML = "";

  int n = WiFi.scanNetworks();
  if (n <= 0) {
    networksHTML = "<option value=''>No networks found</option>";
    Serial.println("No networks found.");
  } else {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;

      networksHTML += "<option value='";
      networksHTML += ssid;
      networksHTML += "'>";
      networksHTML += ssid;
      networksHTML += "</option>";
    }
  }
  WiFi.scanDelete();
}

// ===================== PORTAL HTML =====================
String buildPortalHtml() {
  if (networksHTML.length() == 0) buildNetworksList();

  String html =
    "<!DOCTYPE html><html lang='en'>"
    "<head><meta charset='UTF-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'/>"
    "<title>ESP32-C3 WiFi Config</title>"
    "<style>"
    "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
    ".c{max-width:560px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:20px;}"
    "label{display:block;margin-top:10px;font-size:13px;color:#333;}"
    "input,select{width:100%;padding:10px 12px;border:1px solid #ccc;border-radius:10px;font-size:14px;}"
    "button,a{display:block;width:100%;margin-top:12px;padding:12px;border-radius:12px;border:none;background:#111;color:#fff;"
    "text-decoration:none;text-align:center;font-weight:800;}"
    "a{background:#6b7280;}"
    "</style></head><body>"
    "<div class='c'>"
    "<h2>WiFi Setup</h2>"
    "<form method='POST' action='/save'>"
      "<label>WiFi SSID</label>"
      "<select name='ssid'>" + networksHTML + "</select>"
      "<label>Password</label>"
      "<input type='password' name='pass' placeholder='Enter WiFi password'/>"
      "<label>Django Ingest URL</label>"
      "<input type='text' name='url' value='" + cfgIngestUrl + "'/>"
      "<label>Device ID (optional)</label>"
      "<input type='text' name='device_id' value='" + cfgDeviceId + "'/>"
      "<label>Device Name</label>"
      "<input type='text' name='device_name' value='" + cfgDeviceName + "'/>"
      "<label>Patient Code (X-PUBLIC-CODE)</label>"
      "<input type='text' name='patient_code' value='" + cfgPatientCode + "' placeholder='PUB-XXXXXXX'/>"
      "<button type='submit'>Connect & Save</button>"
    "</form>"
    "<a href='/rescan'>Rescan Networks</a>"
    "<a href='/factory' onclick=\"return confirm('Factory reset?');\">Factory Reset</a>"
    "</div></body></html>";

  return html;
}

void handlePortal() { server.send(200, "text/html", buildPortalHtml()); }

void handleRescan() {
  buildNetworksList();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Rescanning...");
}

void handleFactory() {
  clearSavedWifi();
  clearDeviceConfig();
  server.send(200, "text/plain", "Factory reset done. Rebooting...");
  delay(800);
  ESP.restart();
}

// ===================== CONNECT HELPER =====================
bool connectWiFiWith(const String &ssid, const String &pass, bool keepAP) {
  WiFi.disconnect(true, true);
  delay(300);

  WiFi.mode(keepAP ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(78);

  const int MAX_ATTEMPTS = 3;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    Serial.print("Connecting to '"); Serial.print(ssid);
    Serial.print("' (attempt "); Serial.print(attempt); Serial.println(")...");

    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("✅ WiFi connected");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      return true;
    }
    Serial.println("❌ WiFi failed");
    delay(1500);
  }

  return false;
}

bool connectSavedWifi() {
  String ssid, pass;
  if (!loadSavedWifi(ssid, pass)) return false;
  return connectWiFiWith(ssid, pass, false);
}

// ===================== SENSOR INIT =====================
bool initSensor() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  delay(200);

  const int MAX_TRIES = 5;
  for (int i = 0; i < MAX_TRIES; i++) {
    Serial.print("Trying MAX3010x init, attempt ");
    Serial.println(i + 1);

    if (sensor.begin(Wire, I2C_SPEED_FAST)) {
      Serial.println("✅ MAX3010x found!");

      // ✅ IMPORTANT: use a higher ADC range to avoid 262143 clipping
      byte ledBrightness = 60;
      byte sampleAverage = 4;
      byte ledMode = 2;        // Red + IR
      int  sampleRate = 100;
      int  pulseWidth = 411;
      int  adcRange = 16384;

      sensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);

      // Start moderate (autoGain will tune)
      applyLedAmps();
      sensor.setPulseAmplitudeGreen(0x00);

      return true;
    }

    Serial.println("❌ MAX3010x not found, retrying...");
    delay(300);
  }
  return false;
}

// ===================== SPO2 / PI =====================
void computeSpO2andPIFromWindow() {
  if (windowCount < WINDOW_SIZE) return;

  float irMean = 0.0f, redMean = 0.0f;
  for (int i = 0; i < WINDOW_SIZE; i++) {
    irMean  += irWindow[i];
    redMean += redWindow[i];
  }
  irMean  /= WINDOW_SIZE;
  redMean /= WINDOW_SIZE;

  if (irMean < 2000 || redMean < 2000) {
    hasValidSpO2 = false;
    currentPI = 0.0f;
    return;
  }

  float irAcMean = 0.0f, redAcMean = 0.0f;
  for (int i = 0; i < WINDOW_SIZE; i++) {
    irAcMean  += fabs(irWindow[i]  - irMean);
    redAcMean += fabs(redWindow[i] - redMean);
  }
  irAcMean  /= WINDOW_SIZE;
  redAcMean /= WINDOW_SIZE;

  float pi = (irAcMean / irMean) * 100.0f;
  if (pi < 0.0f)  pi = 0.0f;
  if (pi > 20.0f) pi = 20.0f;
  currentPI = pi;

  float irAcDc  = irAcMean  / irMean;
  float redAcDc = redAcMean / redMean;
  if (irAcDc <= 0.0f) {
    hasValidSpO2 = false;
    return;
  }

  float R    = redAcDc / irAcDc;
  float spo2 = 110.0f - 25.0f * R;

  if (spo2 > 100.0f) spo2 = 100.0f;
  if (spo2 < 70.0f)  spo2 = 70.0f;

  currentSpO2  = spo2;
  hasValidSpO2 = true;
}

// ===================== BEAT DETECTOR (fixed threshold logic) =====================
static bool     emaInit = false;
static float    dcEma = 0.0f;
static float    acAbsEma = 0.0f;
static bool     aboveThr = false;
static float    peakAc = 0.0f;
static uint32_t peakTime = 0;
static uint32_t lastBeatTime = 0;

static inline float fmaxf2(float a, float b) { return (a > b) ? a : b; }

void resetBeatDetector() {
  emaInit = false;
  dcEma = 0.0f;
  acAbsEma = 0.0f;
  aboveThr = false;
  peakAc = 0.0f;
  peakTime = 0;
  lastBeatTime = 0;

  posAbsEma = 0.0f;
  negAbsEma = 0.0f;
  useNegPolarity = false;
}

void onBeat(uint32_t beatTime) {
  if (lastBeatTime == 0) {
    lastBeatTime = beatTime;
    return; // need 2 beats to get dt
  }

  uint32_t dt = beatTime - lastBeatTime;
  if (dt < 250 || dt > 2000) return; // more tolerant window

  float instBpm = 60000.0f / (float)dt;
  if (instBpm < MIN_BPM || instBpm > MAX_BPM) return;

  lastBeatTime = beatTime;

  beatBpmHistory[beatIndex] = instBpm;
  beatIndex = (beatIndex + 1) % BEAT_HISTORY;
  if (beatCount < BEAT_HISTORY) beatCount++;

  float sum = 0.0f;
  for (int i = 0; i < beatCount; i++) sum += beatBpmHistory[i];
  currentBpm = (int)(sum / beatCount + 0.5f);

  // ✅ show BPM as soon as we have 1 interval (2 beats)
  hasValidBpm = (beatCount >= 1);
}

void processIrForBeats(long ir) {
  static uint32_t aboveStartMs = 0;

  if (!emaInit) {
    dcEma = (float)ir;
    acAbsEma = 0.0f;
    posAbsEma = 0.0f;
    negAbsEma = 0.0f;
    useNegPolarity = false;
    aboveThr = false;
    aboveStartMs = 0;
    emaInit = true;
    return;
  }

  // DC removal
  dcEma += 0.02f * ((float)ir - dcEma);
  float ac = (float)ir - dcEma;

  // polarity learning
  float absac = fabs(ac);
  if (ac >= 0) posAbsEma += 0.05f * (absac - posAbsEma);
  else         negAbsEma += 0.05f * (absac - negAbsEma);

  if (!useNegPolarity && negAbsEma > posAbsEma * 1.6f) useNegPolarity = true;
  if ( useNegPolarity && posAbsEma > negAbsEma * 1.6f) useNegPolarity = false;

  // chosen polarity signal
  float x = useNegPolarity ? (-ac) : (ac);

  // adaptive threshold (a bit easier to trigger)
  acAbsEma += 0.05f * (fabs(x) - acAbsEma);
  float thr = fmaxf2(acAbsEma * 0.40f, 12.0f);

  if (!aboveThr) {
    if (x > thr) {
      aboveThr = true;
      aboveStartMs = millis();
      peakAc = x;
      peakTime = millis();
    }
  } else {
    if (x > peakAc) {
      peakAc = x;
      peakTime = millis();
    }

    // ✅ easier release so it re-arms every beat
    bool released = (x < (thr * 0.65f));

    // ✅ safety timeout: if we stayed "above" too long, force release
    bool timedOut = (millis() - aboveStartMs > 420);

    if (released || timedOut) {
      aboveThr = false;

      static uint32_t lastAccepted = 0;
      if (millis() - lastAccepted >= 280) {
        lastAccepted = millis();
        onBeat(peakTime);
      }
    }
  }
}

// ===================== HTTPS POST =====================
int postToDjangoWithSnapshot(const VitalsSnapshot &s) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  if (!cfgPatientCode.length()) return -2;

  String bpmStr  = s.validBpm  ? String(s.bpm)     : "null";
  String sbpStr  = s.validBpm  ? String(s.sbp)     : "null";
  String dbpStr  = s.validBpm  ? String(s.dbp)     : "null";
  String spo2Str = s.validSpO2 ? String(s.spo2, 1) : "null";
  String piStr   = s.finger    ? String(s.pi, 1)   : "null";
  String rrStr   = s.validBpm  ? String(s.rr, 1)   : "null";
  String tmpStr  = s.validTemp ? String(s.temp, 1) : "null";
  String bpStr   = s.validBpm  ? ("\"" + String(s.sbp) + "/" + String(s.dbp) + "\"") : "null";

  String payload = "{";
  payload += "\"device_id\":\"" + cfgDeviceId + "\",";
  payload += "\"ir\":" + String(s.ir) + ",";
  payload += "\"red\":" + String(s.red) + ",";
  payload += "\"finger\":" + String(s.finger ? "true" : "false") + ",";
  payload += "\"bpm\":" + bpmStr + ",";
  payload += "\"hr\":" + bpmStr + ",";
  payload += "\"sbp\":" + sbpStr + ",";
  payload += "\"dbp\":" + dbpStr + ",";
  payload += "\"bp\":" + bpStr + ",";
  payload += "\"spo2\":" + spo2Str + ",";
  payload += "\"pi\":" + piStr + ",";
  payload += "\"rr\":" + rrStr + ",";
  payload += "\"temp\":" + tmpStr;
  payload += "}";

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(true);

  int code = -10;
  String resp;

  if (cfgIngestUrl.startsWith("https://")) {
    tlsClient.setInsecure();
    tlsClient.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(tlsClient, cfgIngestUrl.c_str())) return -10;
  } else {
    if (!http.begin(cfgIngestUrl.c_str())) return -10;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-PUBLIC-CODE", cfgPatientCode);
  if (cfgDeviceName.length()) http.addHeader("X-DEVICE-NAME", cfgDeviceName);

  String esp32Url = "http://" + WiFi.localIP().toString() + "/";
  http.addHeader("X-ESP32-URL", esp32Url);


  code = http.POST(payload);
  if (code > 0) resp = http.getString();
  else resp = http.errorToString(code);

  http.end();

  Serial.print("[Django POST] code=");
  Serial.print(code);
  Serial.print(" resp=");
  Serial.println(resp);

  return code;
}

void postTask(void *param) {
  (void)param;
  uint32_t lastPost = 0;

  for (;;) {
    if (inConfigMode || !sensorReady) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    if (WiFi.status() == WL_CONNECTED && (millis() - lastPost >= POST_INTERVAL_MS)) {
      VitalsSnapshot local;
      portENTER_CRITICAL(&gSnapMux);
      local = gSnap;
      portEXIT_CRITICAL(&gSnapMux);

      postToDjangoWithSnapshot(local);
      lastPost = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ===================== RESET / REBOOT =====================
void handleReset() {
  clearSavedWifi();
  server.send(200, "text/plain", "WiFi cleared. Rebooting...");
  delay(600);
  ESP.restart();
}

void handleReboot() {
  server.send(200, "text/plain", "Rebooting...");
  delay(400);
  ESP.restart();
}

// ===================== STA PAGE =====================
void handleStaRoot() {
  String ip = WiFi.localIP().toString();

  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'/>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'/>"
    "<title>ESP32 Status</title>"
    "<style>"
      "body{font-family:system-ui;background:#f5f5f5;margin:0;}"
      ".card{max-width:720px;margin:40px auto;background:#fff;border:1px solid #ddd;border-radius:18px;padding:18px;}"
      "code{background:#f1f1f1;padding:2px 6px;border-radius:8px;}"
      "label{display:block;margin-top:10px;font-size:13px;color:#333;}"
      "input{width:100%;padding:10px 12px;border:1px solid #ccc;border-radius:10px;font-size:14px;}"
      ".row{display:flex;gap:10px;margin-top:12px;}"
      ".btn{flex:1;display:block;text-align:center;padding:12px;border-radius:12px;background:#111;color:#fff;"
      "text-decoration:none;font-weight:800;border:none;cursor:pointer;}"
      ".btn.red{background:#dc2626;}"
      ".btn.gray{background:#6b7280;}"
      "#toast{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);"
      "background:#111;color:#fff;padding:12px 14px;border-radius:12px;"
      "box-shadow:0 10px 30px rgba(0,0,0,.18);display:none;min-width:240px;"
      "text-align:center;font-weight:800;z-index:9999;}"
      "#toast.ok{background:#16a34a;}"
      "#toast.bad{background:#dc2626;}"
    "</style></head><body>"
    "<div id='toast'></div>"
    "<div class='card'>"
      "<h2>ESP32 Connected ✅</h2>"
      "<p><b>IP:</b> " + ip + "</p>"
      "<p><b>Posting to:</b> <code>" + cfgIngestUrl + "</code></p>"
      "<p><b>Patient Code:</b> <code>" + (cfgPatientCode.length() ? cfgPatientCode : "(none)") + "</code></p>"
      "<hr/>"
      "<h3>Edit Device Settings</h3>"

      "<form id='deviceForm'>"
        "<label>Django Ingest URL</label>"
        "<input name='url' value='" + cfgIngestUrl + "'/>"
        "<label>Device ID</label>"
        "<input name='device_id' value='" + cfgDeviceId + "'/>"
        "<label>Device Name</label>"
        "<input name='device_name' value='" + cfgDeviceName + "'/>"
        "<label>Patient Code (X-PUBLIC-CODE)</label>"
        "<input name='patient_code' value='" + cfgPatientCode + "'/>"

        "<div class='row'>"
          "<button class='btn' type='submit'>Save</button>"
          "<button class='btn red' type='button' onclick='doReset()'>Reset WiFi</button>"
        "</div>"
        "<div class='row'>"
          "<button class='btn gray' type='button' onclick='doReboot()'>Reboot</button>"
          "<button class='btn gray' type='button' onclick='doFactory()'>Factory Reset</button>"
        "</div>"
      "</form>"
    "</div>"

    "<script>"
      "const toastEl=document.getElementById('toast');"
      "function toast(msg, ok=true){"
        "toastEl.className='';"
        "toastEl.classList.add(ok?'ok':'bad');"
        "toastEl.textContent=msg;"
        "toastEl.style.display='block';"
        "clearTimeout(window.__tHide);"
        "window.__tHide=setTimeout(()=>toastEl.style.display='none',1800);"
      "}"

      "async function pingBack(){"
        "let tries=0;"
        "const timer=setInterval(async()=>{"
          "tries++;"
          "try{"
            "const r=await fetch('/ping',{cache:'no-store'});"
            "if(r.ok){ clearInterval(timer); location.href='/'; }"
          "}catch(e){}"
          "if(tries>40){ clearInterval(timer); toast('Still rebooting… refresh', false); }"
        "},500);"
      "}"

      "document.getElementById('deviceForm').addEventListener('submit', async (e)=>{"
        "e.preventDefault();"
        "toast('Saving…');"
        "try{"
          "const fd=new FormData(e.target);"
          "const r=await fetch('/save_device',{method:'POST',body:fd,cache:'no-store'});"
          "toast(r.ok ? '✅ Saved!' : '❌ Save failed', r.ok);"
        "}catch(err){ toast('❌ Save failed', false); }"
      "});"

      "async function doReboot(){"
        "toast('Rebooting…');"
        "try{ fetch('/reboot',{cache:'no-store'}); }catch(e){}"
        "setTimeout(pingBack,700);"
      "}"

      "async function doReset(){"
        "toast('Resetting WiFi…');"
        "try{ fetch('/reset',{cache:'no-store'}); }catch(e){}"
      "}"

      "async function doFactory(){"
        "if(!confirm('Factory reset?')) return;"
        "toast('Factory reset…');"
        "try{ fetch('/factory',{cache:'no-store'}); }catch(e){}"
      "}"
    "</script>"

    "</body></html>";

  server.send(200, "text/html", html);
}

void handleSaveDevice() {
  String url  = server.arg("url"); url.trim();
  String did  = server.arg("device_id"); did.trim();
  String name = server.arg("device_name"); name.trim();
  String pub  = server.arg("patient_code"); pub.trim();

  if (url.length())  cfgIngestUrl  = url;
  if (did.length())  cfgDeviceId   = did;
  if (name.length()) cfgDeviceName = name;
  cfgPatientCode = pub;

  saveDeviceConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNotFound() {
  if (inConfigMode) server.send(200, "text/html", buildPortalHtml());
  else server.send(404, "text/plain", "Not found");
}

// ===================== PORTAL SAVE HANDLER =====================
void handleSaveConfig() {
  String ssid = server.arg("ssid"); ssid.trim();
  String pass = server.arg("pass"); pass.trim();

  String url  = server.arg("url"); url.trim();
  String did  = server.arg("device_id"); did.trim();
  String name = server.arg("device_name"); name.trim();
  String pub  = server.arg("patient_code"); pub.trim();

  bool ok = connectWiFiWith(ssid, pass, true);

  if (ok) {
    saveWifi(ssid, pass);

    if (url.length())  cfgIngestUrl  = url;
    if (did.length())  cfgDeviceId   = did;
    if (name.length()) cfgDeviceName = name;
    cfgPatientCode = pub;

    saveDeviceConfig();

    server.send(200, "text/plain", "Connected & saved. Rebooting...");
    delay(800);
    ESP.restart();
  } else {
    server.send(200, "text/plain", "Failed to connect. Try again.");
  }
}

// ===================== START CONFIG AP =====================
void startConfigAP() {
  Serial.println("Starting CONFIG AP (captive)…");
  inConfigMode = true;

  WiFi.mode(WIFI_AP_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(78);

  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, nullptr, 1, 0, 4);

  dns.start(DNS_PORT, "*", apIP);

  buildNetworksList();

  server.on("/", handlePortal);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/rescan", handleRescan);
  server.on("/factory", handleFactory);

  server.on("/generate_204", handlePortal);
  server.on("/gen_204", handlePortal);
  server.on("/hotspot-detect.html", handlePortal);
  server.on("/ncsi.txt", handlePortal);
  server.on("/connecttest.txt", handlePortal);

  server.onNotFound(handleNotFound);
  server.begin();
}

// ===================== START STA SERVER =====================
void startStaServer() {
  inConfigMode = false;
  dns.stop();

  server.on("/", handleStaRoot);
  server.on("/reset", handleReset);
  server.on("/reboot", handleReboot);
  server.on("/factory", handleFactory);
  server.on("/save_device", HTTP_POST, handleSaveDevice);
  server.on("/ping", [](){ server.send(200, "text/plain", "ok"); });
  server.onNotFound(handleNotFound);

  server.begin();

  Serial.println("✅ STA server started.");
  Serial.print("Open: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

// ===================== SETUP / LOOP =====================
void resetVitalsState() {
  windowIndex  = 0;
  windowCount  = 0;
  beatIndex    = 0;
  beatCount    = 0;
  hasValidBpm  = false;
  hasValidSpO2 = false;
  currentPI    = 0.0f;
  currentRR    = 0.0f;
  currentSBP   = 0;
  currentDBP   = 0;
  resetBeatDetector();
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("=== ESP32-C3 + BPM FIX (no clipping) ===");

  loadDeviceConfig();

#ifdef RESET_SAVED_WIFI_ON_BOOT
  clearSavedWifi();
#endif

  if (!connectSavedWifi()) {
    startConfigAP();
    return;
  }

  startStaServer();

  Serial.println("[BOOT] Warm-up 2s...");
  delay(2000);

  sensorReady = initSensor();
  if (!sensorReady) Serial.println("⚠️ Sensor not ready.");

  Serial.print("Posting to Django: ");
  Serial.println(cfgIngestUrl);

  xTaskCreatePinnedToCore(
    postTask,
    "postTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  if (inConfigMode) {
    dns.processNextRequest();
    server.handleClient();
    delay(10);
    return;
  }

  server.handleClient();
  if (!sensorReady) { delay(10); return; }

  sensor.check();

  static uint32_t noFingerSince = 0;
  static uint32_t lastTempRead = 0;
  static bool fingerState = false;

  // ✅ signal debug window
  static long irMin = 2147483647;
  static long irMax = 0;
  static uint32_t sigStart = 0;

  while (sensor.available()) {
    long ir  = sensor.getIR();
    long red = sensor.getRed();
    sensor.nextSample();

    currentIr = ir;
    currentRed = red;

    // finger hysteresis
    if (!fingerState && ir > FINGER_ON_THRESHOLD) fingerState = true;
    if (fingerState && ir < FINGER_OFF_THRESHOLD) fingerState = false;
    hasFinger = fingerState;

    // temp (once/sec)
    if (millis() - lastTempRead > 1000) {
      float t = sensor.readTemperature();
      if (hasFinger && t > 20.0f && t < 45.0f) { currentTemp = t; hasValidTemp = true; }
      else { hasValidTemp = false; }
      lastTempRead = millis();
    }

    // require 1500ms no-finger before wipe
    if (!hasFinger) {
      if (noFingerSince == 0) noFingerSince = millis();
      if (millis() - noFingerSince > 1500) {
        resetVitalsState();
      }
      continue;
    } else {
      noFingerSince = 0;
    }

    // ✅ auto-gain prevents 262143 saturation
    autoGain(ir);

    // ✅ signal debug min/max
    if (ir < irMin) irMin = ir;
    if (ir > irMax) irMax = ir;
    if (sigStart == 0) sigStart = millis();
    if (millis() - sigStart >= 1000) {
      long delta = (irMax > irMin) ? (irMax - irMin) : 0;
      Serial.print("[SIG] IRmin="); Serial.print(irMin);
      Serial.print(" IRmax="); Serial.print(irMax);
      Serial.print(" delta="); Serial.print(delta);
      Serial.print(" | BPM=");
      if (hasValidBpm) Serial.print(currentBpm); else Serial.print("null");
      Serial.print(" beats="); Serial.print(beatCount);
      Serial.print(" IRamp=0x"); Serial.print(irAmp, HEX);
      Serial.print(" REDamp=0x"); Serial.println(redAmp, HEX);

      irMin = 2147483647;
      irMax = 0;
      sigStart = millis();
    }

    // SpO2/PI window update (optional)
    irWindow[windowIndex]  = (float)ir;
    redWindow[windowIndex] = (float)red;
    windowIndex = (windowIndex + 1) % WINDOW_SIZE;
    if (windowCount < WINDOW_SIZE) windowCount++;
    if (windowCount == WINDOW_SIZE) computeSpO2andPIFromWindow();

    // ✅ Beat detection
    processIrForBeats(ir);

    // RR + BP (depends on BPM)
    if (hasValidBpm) {
      float rr = 16.0f + (currentBpm - 70) * 0.05f;
      if (rr < 8.0f)  rr = 8.0f;
      if (rr > 30.0f) rr = 30.0f;
      currentRR = rr;

      float sys = 110.0f + (currentBpm - 70) * 0.4f;
      float dia = 70.0f  + (currentBpm - 70) * 0.3f;

      if (sys < 90.0f)  sys = 90.0f;
      if (sys > 200.0f) sys = 200.0f;
      if (dia < 50.0f)  dia = 50.0f;
      if (dia > 130.0f) dia = 130.0f;

      currentSBP = (int)(sys + 0.5f);
      currentDBP = (int)(dia + 0.5f);
    }
  }

  // ✅ update snapshot for POST task
  portENTER_CRITICAL(&gSnapMux);
  gSnap.ir        = currentIr;
  gSnap.red       = currentRed;
  gSnap.finger    = hasFinger;
  gSnap.validBpm  = hasValidBpm;
  gSnap.bpm       = currentBpm;
  gSnap.validSpO2 = hasValidSpO2;
  gSnap.spo2      = currentSpO2;
  gSnap.pi        = currentPI;
  gSnap.validTemp = hasValidTemp;
  gSnap.temp      = currentTemp;
  gSnap.rr        = currentRR;
  gSnap.sbp       = currentSBP;
  gSnap.dbp       = currentDBP;
  portEXIT_CRITICAL(&gSnapMux);

  delay(1);
}
