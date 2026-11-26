// =====================================================================
//  Diagnóstico NMEA 2000 — ESP32 + transceptor CAN (SN65HVD230)
//  Crea la Wi-Fi "N2K-Diagnostico" (clave nmea2000) y sirve la web en
//  http://192.168.4.1  o  http://n2k.local
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "driver/twai.h"
#include "n2k_core.h"
#include "web_assets.h"

// ---------------- Configuración ----------------
#define FW_VERSION   "1.1"
#define CAN_TX_PIN   GPIO_NUM_5
#define CAN_RX_PIN   GPIO_NUM_4
#define VSENSE_PIN   34            // divisor 100k / 20k desde NET-S (+12 V)
#define LED_PIN      2
static const float VDIV = (100.0f + 20.0f) / 20.0f;
static const char* AP_SSID = "N2K-Diagnostico";
static const char* AP_PASS = "nmea2000";

// Prototipos (el entorno Arduino no los genera aquí)
void canStart(bool active);
void canTask(void*);
void sendFrame(uint32_t pgn, uint8_t da, const uint8_t* d, uint8_t len, uint8_t prio);
void sendClaim();
void sendRequest(uint32_t pgn, uint8_t da);
void startDiag(uint32_t durMs, bool active);
void handleSnapshot();
void handleStart();
void handleResume();
void sendAsset(const uint8_t* data, size_t len, const char* type, bool cache);
void handleUpdateUpload();
void handleUpdateDone();

// Actualización por Wi-Fi (OTA): el firmware nuevo solo se da por bueno tras
// 30 s funcionando. Si se cuelga o reinicia antes, el ESP32 vuelve al anterior.
bool verifyRollbackLater() { return true; }
bool otaValidated = false;
bool otaOk = false;
String otaErr;

// ---------------- Estado ----------------
n2k::Core* core = nullptr;   // en el heap: es demasiado grande para la RAM estática
SemaphoreHandle_t mtx;
WebServer server(80);

volatile bool activeMode = true;     // modo completo (envía peticiones)
volatile bool canRunning = false;
volatile bool frozen = false;        // diagnóstico terminado: datos congelados
volatile bool diagRunning = false;
uint32_t diagStart = 0, diagDur = 0;
uint32_t claimAt = 0;                // momento del último address claim propio
int reqStep = 0;

// Tensión
float vNow = 0, vMin = 99, vMax = 0; double vSum = 0; uint32_t vN = 0;
// Contadores TWAI (base al iniciar diagnóstico)
twai_status_info_t twBase, twEnd;
uint32_t alBusOff = 0, alErrPass = 0, alRecovered = 0;
const char* twState = "stopped";

// ---------------- CAN ----------------
void canStart(bool active) {
  if (canRunning) { twai_stop(); twai_driver_uninstall(); canRunning = false; }
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, active ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 400;
  g.tx_queue_len = 20;
  g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_ERR_ACTIVE;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) == ESP_OK && twai_start() == ESP_OK) {
    canRunning = true; twState = "running";
  } else {
    twState = "error";
  }
  memset(&twBase, 0, sizeof(twBase));
  twai_get_status_info(&twBase);
}

void sendFrame(uint32_t pgn, uint8_t da, const uint8_t* d, uint8_t len, uint8_t prio) {
  if (!canRunning || !activeMode) return;
  twai_message_t m = {};
  m.extd = 1;
  m.identifier = n2k::Core::makeId(prio, pgn, da, core->ownAddr);
  m.data_length_code = len;
  memcpy(m.data, d, len);
  if (twai_transmit(&m, pdMS_TO_TICKS(5)) == ESP_OK) core->bus.ownTx++; else core->bus.ownTxFail++;
}

void sendClaim() {
  uint8_t d[8];
  for (int i = 0; i < 8; i++) d[i] = (core->ownName >> (8 * i)) & 0xFF;
  sendFrame(60928, 255, d, 8, 6);
  claimAt = millis();
}

void sendRequest(uint32_t pgn, uint8_t da) {
  uint8_t d[3] = { (uint8_t)(pgn & 0xFF), (uint8_t)((pgn >> 8) & 0xFF), (uint8_t)((pgn >> 16) & 0xFF) };
  sendFrame(59904, da, d, 3, 6);
}

// Peticiones activas programadas durante el diagnóstico
void activeSchedule(uint32_t el) {
  struct Step { uint32_t at; int what; };
  static const Step steps[] = { {0, 0}, {400, 1}, {900, 2}, {1400, 3}, {6000, 4}, {12000, 4}, {20000, 5} };
  while (reqStep < (int)(sizeof(steps) / sizeof(steps[0])) && el >= steps[reqStep].at) {
    int w = steps[reqStep].what;
    if (w == 0) sendClaim();
    else if (w == 1) { core->requestsSentAt = millis(); sendRequest(60928, 255); }
    else if (w == 2) { core->requestsSentAt = millis(); sendRequest(126996, 255); }
    else if (w == 3) { sendRequest(126998, 255); }
    else if (w == 4 || w == 5) {
      // a cada equipo que todavía no ha contestado, se lo pedimos directamente
      for (int sa = 0; sa < 252; sa++) {
        n2k::Device& d = core->dev[sa];
        if (!d.seen || sa == core->ownAddr) continue;
        bool hasPi = d.info >= 0 && core->info[d.info].piLen > 0;
        if (w == 4 && !hasPi) { sendRequest(126996, sa); vTaskDelay(pdMS_TO_TICKS(10)); }
        if (w == 5 && d.claims == 0) { core->requestsSentAt = millis(); sendRequest(60928, sa); vTaskDelay(pdMS_TO_TICKS(10)); }
      }
    }
    reqStep++;
  }
}

void canTask(void*) {
  uint32_t lastV = 0;
  for (;;) {
    twai_message_t m;
    if (canRunning && twai_receive(&m, pdMS_TO_TICKS(5)) == ESP_OK) {
      if (m.extd && !m.rtr && !frozen) {
        n2k::Frame f; f.id = m.identifier; f.len = m.data_length_code > 8 ? 8 : m.data_length_code;
        memcpy(f.data, m.data, f.len);
        xSemaphoreTake(mtx, portMAX_DELAY);
        core->onFrame(f, millis());
        xSemaphoreGive(mtx);
      }
      continue;   // vacía la cola antes de hacer otras tareas
    }
    uint32_t now = millis();
    if (!canRunning) vTaskDelay(pdMS_TO_TICKS(5));

    // Alertas del controlador CAN
    uint32_t alerts;
    if (canRunning && twai_read_alerts(&alerts, 0) == ESP_OK) {
      bool count = !frozen;
      if (alerts & TWAI_ALERT_BUS_OFF) { if (count) alBusOff++; twState = "bus-off"; twai_initiate_recovery(); }
      if (alerts & TWAI_ALERT_ERR_PASS) { if (count) alErrPass++; twState = "error-pasivo"; }
      if (alerts & TWAI_ALERT_BUS_RECOVERED) { if (count) alRecovered++; twai_start(); twState = "running"; }
      if (alerts & TWAI_ALERT_ERR_ACTIVE) twState = "running";
    }

    xSemaphoreTake(mtx, portMAX_DELAY);
    if (!frozen) core->tick(now);
    // Dirección propia
    if (activeMode && !frozen) {
      if (core->claimLost) {
        core->claimLost = false;
        uint8_t a = core->ownAddr;
        for (int i = 0; i < 250; i++) { a = (a == 0) ? 251 : a - 1; if (!core->dev[a].seen) break; }
        core->ownAddr = a; sendClaim();
      }
      if (core->answerClaimRequest) { core->answerClaimRequest = false; sendClaim(); }
      if (diagRunning) activeSchedule(now - diagStart);
    }
    // Fin del diagnóstico
    if (diagRunning && now - diagStart >= diagDur) {
      diagRunning = false; frozen = true;
      if (canRunning) twai_get_status_info(&twEnd);   // congela los contadores de errores
    }
    xSemaphoreGive(mtx);

    // Tensión cada 50 ms
    if (now - lastV >= 50 && !frozen) {
      lastV = now;
      uint32_t mv = 0;
      for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(VSENSE_PIN);
      float v = (mv / 4) * VDIV / 1000.0f;
      vNow = v;
      if (v > 4.0f) { if (v < vMin) vMin = v; if (v > vMax) vMax = v; vSum += v; vN++; }
    }
    digitalWrite(LED_PIN, diagRunning ? ((now / 150) & 1) : (core->bus.frames > 0 && ((now / 1000) & 1)));
  }
}

void startDiag(uint32_t durMs, bool active) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  frozen = true;   // pausa mientras se reconfigura
  xSemaphoreGive(mtx);
  if (active != activeMode || !canRunning) { activeMode = active; canStart(active); }
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint32_t now = millis();
  core->reset(now);
  vMin = 99; vMax = 0; vSum = 0; vN = 0;
  alBusOff = alErrPass = alRecovered = 0;
  twai_get_status_info(&twBase);
  diagStart = now; diagDur = durMs; reqStep = 0;
  diagRunning = true; frozen = false;
  xSemaphoreGive(mtx);
}

// ---------------- Web ----------------
static void appendOut(void* ctx, const char* s, size_t n) { ((String*)ctx)->concat(s, n); }

void handleSnapshot() {
  twai_status_info_t st = {};
  if (canRunning) twai_get_status_info(&st);
  if (frozen && diagStart && !diagRunning) st = twEnd;
  uint32_t now = millis();
  char live[640];
  bool vok = vN > 0;
  uint32_t el = diagStart ? ((diagRunning ? now : diagStart + diagDur) - diagStart) : 0;
  snprintf(live, sizeof(live),
    "\"live\":{\"fw\":\"" FW_VERSION "\",\"mode\":\"%s\",\"frozen\":%d,\"up\":%u,"
    "\"diag\":{\"running\":%d,\"elapsed\":%u,\"duration\":%u,\"done\":%d},"
    "\"v\":{\"now\":%.2f,\"min\":%.2f,\"max\":%.2f,\"avg\":%.2f,\"ok\":%d},"
    "\"twai\":{\"state\":\"%s\",\"tec\":%u,\"rec\":%u,\"busErr\":%u,\"missed\":%u,\"overrun\":%u,\"arbLost\":%u,\"txFailed\":%u},"
    "\"alerts\":{\"busOff\":%u,\"errPass\":%u,\"recovered\":%u},\"ownTx\":%u,\"ownTxFail\":%u}",
    activeMode ? "active" : "passive", frozen ? 1 : 0, (unsigned)(now / 1000),
    diagRunning ? 1 : 0, (unsigned)el, (unsigned)diagDur, (!diagRunning && frozen && diagStart) ? 1 : 0,
    vNow, vok ? vMin : 0, vok ? vMax : 0, vok ? (float)(vSum / vN) : 0, vok ? 1 : 0,
    twState, (unsigned)st.tx_error_counter, (unsigned)st.rx_error_counter,
    (unsigned)(st.bus_error_count - twBase.bus_error_count), (unsigned)(st.rx_missed_count - twBase.rx_missed_count),
    (unsigned)(st.rx_overrun_count - twBase.rx_overrun_count), (unsigned)(st.arb_lost_count - twBase.arb_lost_count),
    (unsigned)(st.tx_failed_count - twBase.tx_failed_count),
    (unsigned)alBusOff, (unsigned)alErrPass, (unsigned)alRecovered,
    (unsigned)core->bus.ownTx, (unsigned)core->bus.ownTxFail);
  String out;
  out.reserve(24000);
  xSemaphoreTake(mtx, portMAX_DELAY);
  // si está congelado, el "ahora" del análisis es el final del diagnóstico
  uint32_t t = (frozen && diagStart && !diagRunning) ? diagStart + diagDur : now;
  core->writeJson(appendOut, &out, t, live);
  xSemaphoreGive(mtx);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleStart() {
  uint32_t dur = server.hasArg("dur") ? server.arg("dur").toInt() : 60;
  if (dur < 10) dur = 10; if (dur > 900) dur = 900;
  bool active = !(server.hasArg("mode") && server.arg("mode") == "passive");
  startDiag(dur * 1000, active);
  server.send(200, "application/json", "{\"ok\":1}");
}

void handleResume() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  core->reset(millis()); vMin = 99; vMax = 0; vSum = 0; vN = 0;
  alBusOff = alErrPass = alRecovered = 0;
  twai_get_status_info(&twBase);
  diagRunning = false; diagStart = 0; frozen = false;
  xSemaphoreGive(mtx);
  server.send(200, "application/json", "{\"ok\":1}");
}

void sendAsset(const uint8_t* data, size_t len, const char* type, bool cache) {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", cache ? "max-age=604800" : "no-cache");
  server.send_P(200, type, (const char*)data, len);
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaOk = false; otaErr = "";
    xSemaphoreTake(mtx, portMAX_DELAY); frozen = true; diagRunning = false; xSemaphoreGive(mtx);  // pausa el análisis
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaErr.length() == 0 && Update.write(up.buf, up.currentSize) != up.currentSize) otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaErr.length() == 0) { if (Update.end(true)) otaOk = true; else otaErr = Update.errorString(); }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaErr = "Subida cancelada";
  }
}

void handleUpdateDone() {
  if (otaOk) {
    server.send(200, "text/plain", "OK");
    delay(800);
    ESP.restart();
  } else {
    server.send(500, "text/plain", otaErr.length() ? otaErr : String("Archivo no valido"));
    xSemaphoreTake(mtx, portMAX_DELAY); frozen = false; xSemaphoreGive(mtx);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);
  mtx = xSemaphoreCreateMutex();
  core = new n2k::Core();

  uint8_t mac[6]; WiFi.macAddress(mac);
  uint32_t uniq = ((uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]) & 0x1FFFFF;
  // NAME: fabricante 2046 (sin registrar), función 130 Diagnóstico, clase 10 Herramientas, industria marina
  core->ownName = (uint64_t)uniq | ((uint64_t)2046 << 21) | ((uint64_t)130 << 40) | ((uint64_t)10 << 49) | ((uint64_t)4 << 60) | ((uint64_t)1 << 63);
  core->reset(millis());

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  MDNS.begin("n2k");
  MDNS.addService("http", "tcp", 80);

  server.on("/", []() { sendAsset(INDEX_HTML_GZ, sizeof(INDEX_HTML_GZ), "text/html; charset=utf-8", false); });
  server.on("/jspdf.js", []() { sendAsset(JSPDF_JS_GZ, sizeof(JSPDF_JS_GZ), "application/javascript", true); });
  server.on("/autotable.js", []() { sendAsset(AUTOTABLE_JS_GZ, sizeof(AUTOTABLE_JS_GZ), "application/javascript", true); });
  server.on("/api/snapshot", handleSnapshot);
  server.on("/api/start", handleStart);
  server.on("/api/resume", handleResume);
  server.on("/update", []() { sendAsset(UPDATE_HTML_GZ, sizeof(UPDATE_HTML_GZ), "text/html; charset=utf-8", false); });
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302); });
  server.begin();

  canStart(true);
  xTaskCreatePinnedToCore(canTask, "can", 8192, nullptr, 5, nullptr, 0);
  Serial.printf("N2K Diagnostico listo. Wi-Fi %s  ->  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();
  if (!otaValidated && millis() > 30000) {   // lleva 30 s funcionando: este firmware es bueno
    esp_ota_mark_app_valid_cancel_rollback();
    otaValidated = true;
  }
  delay(2);
}
