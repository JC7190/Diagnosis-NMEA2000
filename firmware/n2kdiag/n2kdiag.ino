// =====================================================================
//  PoweryMax Nmea2000 Diagnostic — ESP32 + transceptor CAN (SN65HVD230)
//  Crea la Wi-Fi "PoweryMax Nmea2000 Diagnostic" (clave nmea2000) y sirve la web en
//  http://192.168.4.1  o  http://powerymax.local
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "driver/twai.h"
#include "n2k_core.h"
#include "web_assets.h"

// ---------------- Configuración ----------------
#define FW_VERSION   "1.8"
#define CAN_TX_PIN   GPIO_NUM_5
#define CAN_RX_PIN   GPIO_NUM_4
#define VSENSE_PIN   34            // divisor 100k / 20k desde NET-S (+12 V)
#define LED_PIN      2
static const float VDIV = (100.0f + 20.0f) / 20.0f;
static const char* AP_SSID = "PoweryMax Nmea2000 Diagnostic";
static const char* AP_PASS = "nmea2000";

// Prototipos (el entorno Arduino no los genera aquí)
void canStart(bool active);
void canTask(void*);
void sendFrame(uint32_t pgn, uint8_t da, const uint8_t* d, uint8_t len, uint8_t prio);
void sendClaim();
void sendRequest(uint32_t pgn, uint8_t da);
void sendProductInfo();
void sendNak(uint32_t pgn, uint8_t to);
void activeSchedule(uint32_t el, uint32_t now);
void applyCommand(uint32_t now);
bool runCommand(int cmd, uint32_t durMs, bool active);
void handleSnapshot();
void handleStart();
void handleResume();
void handleFrames();
void sendAsset(const uint8_t* data, size_t len, const char* type, bool cache);
void handleUpdateUpload();
void handleUpdateDone();

// Actualización por Wi-Fi (OTA): el firmware nuevo solo se da por bueno tras
// 30 s funcionando. Si se cuelga o reinicia antes, el ESP32 vuelve al anterior.
bool verifyRollbackLater() { return true; }
bool otaValidated = false;
bool otaOk = false;
bool otaWasFrozen = false;
String otaErr;

// ---------------- Estado ----------------
n2k::Core* core = nullptr;   // en el heap: es demasiado grande para la RAM estática
SemaphoreHandle_t mtx;
WebServer server(80);
uint32_t httpReqs = 0;   // peticiones atendidas (para el registro por USB)

// Todo lo que toca el driver TWAI se hace SOLO en la tarea CAN. La web pide los
// cambios con un comando y espera a que la tarea lo aplique.
enum { CMD_NONE = 0, CMD_START, CMD_RESUME, CMD_PAUSE };
volatile int cmdPending = CMD_NONE;
volatile uint32_t cmdDurMs = 0;
volatile bool cmdActive = false;
volatile uint32_t cmdSeq = 0, cmdDone = 0;

volatile bool activeMode = false;    // al arrancar solo escucha: no transmite nada hasta que se pida
volatile bool canRunning = false;
volatile bool frozen = false;        // diagnóstico terminado: datos congelados
volatile bool diagRunning = false;
bool claimed = false;                // hemos reclamado dirección en este diagnóstico
uint32_t diagStart = 0, diagDur = 0;
int reqStep = 0;
// Peticiones dirigidas pendientes (se envían de una en una, sin bloquear)
static const int REQ_Q = 512;
struct Req { uint8_t sa; uint8_t kind; };   // kind: 0 = 60928, 1 = 126996, 2 = 126998
Req reqQueue[REQ_Q]; int reqQHead = 0, reqQTail = 0;
static const uint32_t REQ_PGN[3] = { 60928, 126996, 126998 };
uint32_t reqLastMs = 0;

// Tensión (protegida por el mutex)
float vNow = 0, vMin = 99, vMax = 0, vEma = 0, vDev = 0; double vSum = 0; uint32_t vN = 0;
// Contadores TWAI: base al iniciar, último leído y congelado al terminar (protegidos por el mutex)
twai_status_info_t twBase, twNow, twEnd;
uint32_t alBusOff = 0, alErrPass = 0, alRecovered = 0;
const char* twState = "parado";

// ---------------- CAN (solo desde la tarea CAN) ----------------
void canStart(bool active) {
  if (canRunning) { twai_stop(); twai_driver_uninstall(); canRunning = false; }
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, active ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len = 512;
  g.tx_queue_len = active ? 24 : 1;
  g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_ERR_ACTIVE;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t e1 = twai_driver_install(&g, &t, &f);
  esp_err_t e2 = e1 == ESP_OK ? twai_start() : ESP_FAIL;
  if (e1 == ESP_OK && e2 == ESP_OK) {
    canRunning = true; twState = "funcionando";
  } else {
    if (e1 == ESP_OK) twai_driver_uninstall();
    twState = "error";
    Serial.printf("[can] no arranca: instalar=%s iniciar=%s modo=%s heap=%u\n", esp_err_to_name(e1), esp_err_to_name(e2),
                  active ? "normal" : "escucha", (unsigned)ESP.getFreeHeap());
  }
  memset(&twBase, 0, sizeof(twBase));
  if (canRunning) twai_get_status_info(&twBase);
  twNow = twBase;
}

void sendFrame(uint32_t pgn, uint8_t da, const uint8_t* d, uint8_t len, uint8_t prio) {
  if (!canRunning || !activeMode) return;   // en modo escucha nunca se transmite
  twai_message_t m = {};
  m.extd = 1;
  m.identifier = n2k::Core::makeId(prio, pgn, da, core->ownAddr);
  m.data_length_code = len;
  memcpy(m.data, d, len);
  if (twai_transmit(&m, 0) == ESP_OK) core->bus.ownTx++; else core->bus.ownTxFail++;
}

void sendClaim() {
  uint8_t d[8];
  for (int i = 0; i < 8; i++) d[i] = (core->ownName >> (8 * i)) & 0xFF;
  sendFrame(60928, 255, d, 8, 6);
  core->noteOwnClaim(millis());
  claimed = true;
}

// Información de producto propia (126996, fast packet de 134 bytes): así las
// pantallas muestran la herramienta correctamente en su lista de equipos
void sendProductInfo() {
  uint8_t pi[134];
  memset(pi, 0xFF, sizeof(pi));
  pi[0] = 2100 & 0xFF; pi[1] = 2100 >> 8;          // versión NMEA 2000 (2.100)
  pi[2] = 0xFF; pi[3] = 0xFF;                        // código de producto: sin asignar
  auto put = [&](int o, const char* t) { for (int i = 0; i < 32 && t[i]; i++) pi[o + i] = t[i]; };
  char serial[17]; snprintf(serial, sizeof(serial), "%06X", (unsigned)(core->ownName & 0x1FFFFF));
  put(4, "PoweryMax N2K Diag"); put(36, FW_VERSION); put(68, "ESP32"); put(100, serial);
  pi[132] = 0;   // nivel de certificación
  pi[133] = 3;   // consumo: 3 LEN (150 mA)
  static uint8_t seq = 0;
  seq = (seq + 1) & 7;
  uint8_t f[8]; int idx = 0, p = 0;
  while (p < (int)sizeof(pi)) {
    f[0] = (seq << 5) | idx;
    int k = 1;
    if (idx == 0) { f[1] = sizeof(pi); k = 2; }
    for (; k < 8; k++) f[k] = p < (int)sizeof(pi) ? pi[p++] : 0xFF;
    if (!canRunning || !activeMode) return;
    twai_message_t m = {};
    m.extd = 1; m.identifier = n2k::Core::makeId(6, 126996, 255, core->ownAddr); m.data_length_code = 8;
    memcpy(m.data, f, 8);
    if (twai_transmit(&m, pdMS_TO_TICKS(3)) == ESP_OK) core->bus.ownTx++; else { core->bus.ownTxFail++; return; }
    idx++;
  }
}

// Respuesta "no disponible" (ISO Acknowledgement 59392, NAK) a una petición dirigida
void sendNak(uint32_t pgn, uint8_t to) {
  uint8_t d[8] = { 1, 0xFF, 0xFF, 0xFF, to, (uint8_t)(pgn & 0xFF), (uint8_t)((pgn >> 8) & 0xFF), (uint8_t)((pgn >> 16) & 0xFF) };
  sendFrame(59392, to, d, 8, 6);
}

void sendRequest(uint32_t pgn, uint8_t da) {
  uint8_t d[3] = { (uint8_t)(pgn & 0xFF), (uint8_t)((pgn >> 8) & 0xFF), (uint8_t)((pgn >> 16) & 0xFF) };
  if (pgn == 60928) core->noteClaimRequest(millis());
  sendFrame(59904, da, d, 3, 6);
}

static void queueRequest(uint8_t sa, uint32_t pgn) {
  int nx = (reqQTail + 1) & (REQ_Q - 1);
  if (nx == reqQHead) return;
  reqQueue[reqQTail].sa = sa; reqQueue[reqQTail].kind = pgn == 60928 ? 0 : pgn == 126996 ? 1 : 2;
  reqQTail = nx;
}

// ¿Sigue haciendo falta pedírselo? (puede haber contestado mientras esperaba en la cola)
static bool stillMissing(uint8_t sa, int kind) {
  n2k::Device& d = core->dev[sa];
  if (kind == 0) return d.claims == 0;
  if (d.info < 0) return true;
  return kind == 1 ? core->info[d.info].piLen == 0 : core->info[d.info].ciLen == 0;
}

// Peticiones activas programadas durante el diagnóstico (con el mutex tomado)
void activeSchedule(uint32_t el, uint32_t now) {
  struct Step { uint32_t at; int what; };
  static const Step steps[] = { {0, 0}, {400, 1}, {900, 2}, {1400, 3}, {6000, 4}, {12000, 4}, {20000, 5} };
  while (reqStep < (int)(sizeof(steps) / sizeof(steps[0])) && el >= steps[reqStep].at) {
    int w = steps[reqStep].what;
    if (w == 0) sendClaim();
    else if (w == 1) { core->requestsSentAt = now; sendRequest(60928, 255); }
    else if (w == 2 || w == 3) {
      // Información de producto / configuración. En redes pequeñas, una petición a
      // todos; en las grandes, una a una y espaciadas: si contestan 100 equipos a la
      // vez con mensajes largos se satura la red y los mensajes llegan rotos.
      uint32_t pgn = w == 2 ? 126996 : 126998;
      int nDev = 0;
      for (int sa = 0; sa < 252; sa++) if (core->dev[sa].seen && sa != core->ownAddr) nDev++;
      if (nDev <= 24) sendRequest(pgn, 255);
      else for (int sa = 0; sa < 252; sa++) if (core->dev[sa].seen && sa != core->ownAddr) queueRequest(sa, pgn);
    }
    else {
      // a cada equipo que todavía no ha contestado, se lo pedimos directamente
      for (int sa = 0; sa < 252; sa++) {
        n2k::Device& d = core->dev[sa];
        if (!d.seen || sa == core->ownAddr) continue;
        bool hasPi = d.info >= 0 && core->info[d.info].piLen > 0;
        if (w == 4 && !hasPi) queueRequest(sa, 126996);
        if (w == 5 && d.claims == 0) queueRequest(sa, 60928);
      }
    }
    reqStep++;
  }
  // una petición dirigida cada 20 ms como mucho: no satura la red ni la cola de envío
  while (reqQHead != reqQTail && now - reqLastMs >= 20) {
    Req r = reqQueue[reqQHead];
    reqQHead = (reqQHead + 1) & (REQ_Q - 1);
    if (!stillMissing(r.sa, r.kind)) continue;
    if (r.kind == 0) core->requestsSentAt = now;
    sendRequest(REQ_PGN[r.kind], r.sa);
    reqLastMs = now;
  }
}

// ---------------- Captura de tramas ----------------
// Búfer circular con las últimas tramas de la prueba. El iPhone lo va descargando
// (/api/frames) y al final guarda la captura completa en CSV.
struct CapFrame { uint32_t us; uint32_t id; uint8_t len; uint8_t d[8]; };
static const uint32_t CAP_N = 1024;   // ≈ 20 KB; el iPhone pide cada 0,4 s
CapFrame* cap = nullptr;
uint32_t capSeq = 0;                  // tramas capturadas desde el inicio de la prueba
int64_t capT0 = 0;

static void resetCounters(uint32_t now) {
  capSeq = 0; capT0 = esp_timer_get_time();
  core->reset(now);
  vMin = 99; vMax = 0; vSum = 0; vN = 0; vDev = 0;
  alBusOff = alErrPass = alRecovered = 0;
  if (canRunning) twai_get_status_info(&twBase);
  twNow = twBase;
  reqStep = 0; reqQHead = reqQTail = 0; reqLastMs = 0;
  claimed = false;
}

// Ejecuta en la tarea CAN el comando pedido desde la web
void applyCommand(uint32_t now) {
  int cmd = cmdPending;
  if (cmd == CMD_NONE) return;
  if (cmd == CMD_START) {
    bool active = cmdActive;
    if (active != activeMode || !canRunning) { activeMode = active; canStart(active); }
    xSemaphoreTake(mtx, portMAX_DELAY);
    now = millis();
    resetCounters(now);
    diagStart = now; diagDur = cmdDurMs;
    diagRunning = true; frozen = false;
    xSemaphoreGive(mtx);
  } else if (cmd == CMD_RESUME) {
    // vuelve a la vista en directo: solo escuchar (no transmite)
    if (activeMode || !canRunning) { activeMode = false; canStart(false); }
    xSemaphoreTake(mtx, portMAX_DELAY);
    resetCounters(millis());
    diagRunning = false; diagStart = 0; frozen = false;
    xSemaphoreGive(mtx);
  } else if (cmd == CMD_PAUSE) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    frozen = true; diagRunning = false;
    xSemaphoreGive(mtx);
  }
  cmdPending = CMD_NONE;
  cmdDone = cmdSeq;
}

void canTask(void*) {
  uint32_t lastV = 0, lastSt = 0;
  canStart(false);   // la ISR queda en este núcleo
  static n2k::Frame batch[32];
  static uint32_t batchUs[32];
  for (;;) {
    // 1) Recibe un lote de tramas (espera como mucho 2 ms a la primera)
    int n = 0;
    if (canRunning) {
      twai_message_t m;
      TickType_t wait = pdMS_TO_TICKS(2);
      while (n < 32 && twai_receive(&m, wait) == ESP_OK) {
        wait = 0;
        if (!m.extd || m.rtr) continue;
        n2k::Frame& f = batch[n];
        batchUs[n++] = (uint32_t)(esp_timer_get_time() - capT0);
        f.id = m.identifier; f.len = m.data_length_code > 8 ? 8 : m.data_length_code;
        memcpy(f.data, m.data, f.len);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    uint32_t now = millis();

    // 2) Alertas del controlador CAN
    uint32_t alerts = 0;
    if (canRunning && twai_read_alerts(&alerts, 0) == ESP_OK) {
      bool count = !frozen;
      if (alerts & TWAI_ALERT_BUS_OFF) { if (count) alBusOff++; twState = "bus-off"; twai_initiate_recovery(); }
      if (alerts & TWAI_ALERT_ERR_PASS) { if (count) alErrPass++; twState = "error-pasivo"; }
      if (alerts & TWAI_ALERT_BUS_RECOVERED) { if (count) alRecovered++; twai_start(); twState = "funcionando"; }
      if (alerts & TWAI_ALERT_ERR_ACTIVE) twState = "funcionando";
    }
    twai_status_info_t st = {};
    bool gotSt = canRunning && now - lastSt >= 100 && twai_get_status_info(&st) == ESP_OK;
    if (gotSt) lastSt = now;

    // 3) Análisis y peticiones (con el mutex tomado poco tiempo)
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (!frozen) for (int i = 0; i < n; i++) {
      core->onFrame(batch[i], now);
      if (cap && diagRunning) {
        CapFrame& c = cap[capSeq % CAP_N];
        c.us = batchUs[i]; c.id = batch[i].id; c.len = batch[i].len; memcpy(c.d, batch[i].data, 8);
        capSeq++;
      }
    }
    if (gotSt && !frozen) twNow = st;
    if (!frozen) core->tick(now);
    if (activeMode && !frozen && diagRunning) {
      if (core->claimLost) {
        core->claimLost = false;
        uint8_t a = core->ownAddr;
        for (int i = 0; i < 250; i++) { a = (a == 0) ? 251 : a - 1; if (!core->dev[a].seen) break; }
        core->ownAddr = a; sendClaim();
      }
      // solo contestamos si ya hemos reclamado dirección en este diagnóstico
      if (core->answerClaimRequest) { core->answerClaimRequest = false; if (claimed) sendClaim(); }
      // tras reclamar dirección (y los 250 ms que marca la norma) atendemos las peticiones
      if (claimed && now - diagStart >= 300) {
        if (core->answerPiRequest) { core->answerPiRequest = false; sendProductInfo(); }
        if (core->nakPgn) { sendNak(core->nakPgn, core->nakTo); core->nakPgn = 0; }
      }
      activeSchedule(now - diagStart, now);
    } else {
      core->answerClaimRequest = false; core->claimLost = false; core->answerPiRequest = false; core->nakPgn = 0;
    }
    // Fin del diagnóstico
    if (diagRunning && now - diagStart >= diagDur) {
      diagRunning = false; frozen = true;
      if (canRunning) twai_get_status_info(&twNow);
      twEnd = twNow;   // congela los contadores de errores
    }
    xSemaphoreGive(mtx);

    // 4) Comandos de la web
    applyCommand(now);

    // 5) Tensión cada 50 ms (media de 8 lecturas calibradas)
    if (now - lastV >= 50 && !frozen) {
      lastV = now;
      uint32_t mv = 0;
      for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(VSENSE_PIN);
      float v = (mv / 8) * VDIV / 1000.0f;
      xSemaphoreTake(mtx, portMAX_DELAY);
      vNow = v;
      if (v > 4.0f) {
        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
        // inestabilidad = salto respecto a la media del último segundo (un mal
        // contacto da saltos bruscos; la carga del alternador sube despacio)
        if (vN == 0) vEma = v;
        float dv = fabsf(v - vEma);
        if (vN >= 20 && dv > vDev) vDev = dv;
        vEma += (v - vEma) * 0.05f;
        vSum += v; vN++;
      }
      xSemaphoreGive(mtx);
    }
    digitalWrite(LED_PIN, diagRunning ? ((now / 150) & 1) : (core->bus.frames > 0 && ((now / 1000) & 1)));
    // cede SIEMPRE 1 ms: la web (loop, mismo núcleo, menos prioridad) nunca se queda sin turno.
    // Con lotes de 32 tramas por ms se procesa mucho más de lo que cabe en una red a 250 kbit/s.
    vTaskDelay(1);
  }
}

// Pide un comando a la tarea CAN y espera (máx. 1,5 s) a que lo aplique
bool runCommand(int cmd, uint32_t durMs, bool active) {
  cmdDurMs = durMs; cmdActive = active;
  uint32_t seq = cmdSeq + 1;
  cmdSeq = seq;
  cmdPending = cmd;
  uint32_t t0 = millis();
  while (cmdDone != seq && millis() - t0 < 1500) delay(2);
  return cmdDone == seq;
}

// ---------------- Web ----------------
static void chunkOut(void*, const char* s, size_t n) { server.chunkWrite(s, n); }
static void lockFn(void*, bool on) { if (on) xSemaphoreTake(mtx, portMAX_DELAY); else xSemaphoreGive(mtx); }

void handleSnapshot() {
  httpReqs++;
  bool lite = server.hasArg("lite");
  uint32_t now = millis();
  char live[700];
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool done = frozen && diagStart && !diagRunning;
  twai_status_info_t st = done ? twEnd : twNow;
  bool vok = vN > 0;
  uint32_t el = diagStart ? ((diagRunning ? now : diagStart + diagDur) - diagStart) : 0;
  // si está congelado, el "ahora" del análisis es el final del diagnóstico
  uint32_t t = done ? diagStart + diagDur : now;
  snprintf(live, sizeof(live),
    "\"live\":{\"fw\":\"" FW_VERSION "\",\"mode\":\"%s\",\"frozen\":%d,\"up\":%u,"
    "\"diag\":{\"running\":%d,\"elapsed\":%u,\"duration\":%u,\"done\":%d},"
    "\"v\":{\"now\":%.2f,\"min\":%.2f,\"max\":%.2f,\"avg\":%.2f,\"dev\":%.2f,\"ok\":%d,\"n\":%u},"
    "\"twai\":{\"state\":\"%s\",\"tec\":%u,\"rec\":%u,\"busErr\":%u,\"missed\":%u,\"overrun\":%u,\"arbLost\":%u,\"txFailed\":%u},"
    "\"alerts\":{\"busOff\":%u,\"errPass\":%u,\"recovered\":%u},\"ownTx\":%u,\"ownTxFail\":%u,\"heap\":%u}",
    activeMode ? "active" : "passive", frozen ? 1 : 0, (unsigned)(now / 1000),
    diagRunning ? 1 : 0, (unsigned)el, (unsigned)diagDur, done ? 1 : 0,
    vNow, vok ? vMin : 0, vok ? vMax : 0, vok ? (float)(vSum / vN) : 0, vok ? vDev : 0, vok ? 1 : 0, (unsigned)vN,
    twState, (unsigned)st.tx_error_counter, (unsigned)st.rx_error_counter,
    (unsigned)(st.bus_error_count - twBase.bus_error_count), (unsigned)(st.rx_missed_count - twBase.rx_missed_count),
    (unsigned)(st.rx_overrun_count - twBase.rx_overrun_count), (unsigned)(st.arb_lost_count - twBase.arb_lost_count),
    (unsigned)(st.tx_failed_count - twBase.tx_failed_count),
    (unsigned)alBusOff, (unsigned)alErrPass, (unsigned)alRecovered,
    (unsigned)core->bus.ownTx, (unsigned)core->bus.ownTxFail, (unsigned)ESP.getFreeHeap());
  xSemaphoreGive(mtx);
  // Se envía por trozos: no hace falta un búfer enorme y el mutex solo se toma
  // para copiar cada registro (la tarea CAN no se queda esperando a la Wi-Fi)
  server.sendHeader("Cache-Control", "no-store");
  server.chunkResponseBegin("application/json");
  core->writeJson(chunkOut, nullptr, t, live, lockFn, nullptr, lite);
  server.chunkResponseEnd();
}

void handleStart() {
  long d = server.hasArg("dur") ? server.arg("dur").toInt() : 60;
  if (d < 10) d = 10;
  if (d > 900) d = 900;
  bool active = !(server.hasArg("mode") && server.arg("mode") == "passive");
  if (runCommand(CMD_START, (uint32_t)d * 1000, active)) server.send(200, "application/json", "{\"ok\":1}");
  else server.send(503, "application/json", "{\"ok\":0}");
}

// Tramas capturadas a partir de ?from=N (texto: una línea por trama, formato del CSV).
// Primera línea: "#seq=<total>;lost=<perdidas>" (perdidas = se sobrescribieron antes de pedirlas).
void handleFrames() {
  uint32_t from = server.hasArg("from") ? (uint32_t)server.arg("from").toInt() : 0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  uint32_t total = capSeq;
  xSemaphoreGive(mtx);
  uint32_t first = from, lost = 0;
  if (total > CAP_N && first < total - CAP_N) { lost = total - CAP_N - first; first = total - CAP_N; }
  if (first > total) first = total;
  if (total - first > 1200) total = first + 1200;   // como mucho 1200 por petición
  server.sendHeader("Cache-Control", "no-store");
  server.chunkResponseBegin("text/plain");
  char line[64];
  int n = snprintf(line, sizeof(line), "#seq=%u;lost=%u\n", (unsigned)total, (unsigned)lost);
  server.chunkWrite(line, n);
  static CapFrame tmp[48];
  static char out[48 * 44];
  for (uint32_t i = first; cap && i < total; ) {
    uint32_t k = 0;
    xSemaphoreTake(mtx, portMAX_DELAY);
    // si mientras tanto se ha sobrescrito, se para aquí (el iPhone lo pedirá como perdido)
    if (capSeq - i > CAP_N) { xSemaphoreGive(mtx); break; }
    for (; k < 48 && i + k < total; k++) tmp[k] = cap[(i + k) % CAP_N];
    xSemaphoreGive(mtx);
    int len = 0;
    static const char H[] = "0123456789ABCDEF";
    for (uint32_t j = 0; j < k; j++) {
      CapFrame& c = tmp[j];
      len += snprintf(out + len, 24, "%u;%08X;%u;", (unsigned)c.us, (unsigned)(c.id & 0x1FFFFFFF), c.len);
      for (int b = 0; b < c.len; b++) { out[len++] = H[c.d[b] >> 4]; out[len++] = H[c.d[b] & 15]; }
      out[len++] = ';'; out[len++] = '\n';
    }
    server.chunkWrite(out, len);
    i += k;
  }
  server.chunkResponseEnd();
}

void handleResume() {
  runCommand(CMD_RESUME, 0, false);
  server.send(200, "application/json", "{\"ok\":1}");
}

void sendAsset(const uint8_t* data, size_t len, const char* type, bool cache) {
  httpReqs++;
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", cache ? "max-age=604800, immutable" : "no-cache");
  server.send_P(200, type, (const char*)data, len);
}

static String otaErrorText() {
  switch (Update.getError()) {
    case UPDATE_ERROR_MAGIC_BYTE: return "El archivo no es un firmware de ESP32 (¿has elegido el .bin completo en vez del de actualización?)";
    case UPDATE_ERROR_SPACE: case UPDATE_ERROR_SIZE: return "El archivo es demasiado grande para este aparato";
    case UPDATE_ERROR_READ: case UPDATE_ERROR_ACTIVATE: return "El firmware está dañado o incompleto. Vuelve a descargarlo";
    case UPDATE_ERROR_WRITE: case UPDATE_ERROR_ERASE: return "Error al escribir en la memoria del aparato";
    case UPDATE_ERROR_ABORT: return "Actualización cancelada";
    default: return String("Error de actualización (") + Update.errorString() + ")";
  }
}

void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaOk = false; otaErr = "";
    otaWasFrozen = frozen;
    runCommand(CMD_PAUSE, 0, false);   // pausa el análisis
    if (Update.isRunning()) Update.abort();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) otaErr = otaErrorText();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaErr.length() == 0 && Update.write(up.buf, up.currentSize) != up.currentSize) {
      otaErr = otaErrorText(); Update.abort();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaErr.length() == 0) {
      if (up.totalSize < 100000) { otaErr = "Archivo demasiado pequeño: no es un firmware"; Update.abort(); }
      else if (Update.end(true)) otaOk = true;
      else otaErr = otaErrorText();
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaErr = "Subida cancelada";
  }
}

void handleUpdateDone() {
  if (otaOk) {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain; charset=utf-8", "OK");
    delay(800);
    ESP.restart();
  } else {
    server.send(500, "text/plain; charset=utf-8", otaErr.length() ? otaErr : String("Archivo no válido"));
    // el análisis queda congelado; si estaba en directo, vuelve a la vista en directo
    if (!otaWasFrozen) runCommand(CMD_RESUME, 0, false);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);
  mtx = xSemaphoreCreateMutex();
  core = new n2k::Core();
  cap = (CapFrame*)malloc(sizeof(CapFrame) * CAP_N);   // ≈ 20 KB; sin ella, la prueba funciona igual

  uint8_t mac[6]; WiFi.macAddress(mac);
  uint32_t uniq = ((uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5]) & 0x1FFFFF;
  // NAME: fabricante 2046 (sin registrar), función 130 Diagnóstico, clase 10 Herramientas, industria marina
  core->ownName = (uint64_t)uniq | ((uint64_t)2046 << 21) | ((uint64_t)130 << 40) | ((uint64_t)10 << 49) | ((uint64_t)4 << 60) | ((uint64_t)1 << 63);
  core->reset(millis());

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);   // respuesta más rápida en el iPhone
  MDNS.begin("powerymax");
  MDNS.addService("http", "tcp", 80);

  server.on("/", []() { sendAsset(INDEX_HTML_GZ, sizeof(INDEX_HTML_GZ), "text/html; charset=utf-8", false); });
  server.on("/jspdf.js", []() { sendAsset(JSPDF_JS_GZ, sizeof(JSPDF_JS_GZ), "application/javascript", true); });
  server.on("/brand.js", []() { sendAsset(BRAND_JS_GZ, sizeof(BRAND_JS_GZ), "application/javascript", true); });
  server.on("/autotable.js", []() { sendAsset(AUTOTABLE_JS_GZ, sizeof(AUTOTABLE_JS_GZ), "application/javascript", true); });
  server.on("/api/snapshot", handleSnapshot);
  server.on("/api/start", handleStart);
  server.on("/api/resume", handleResume);
  server.on("/api/frames", handleFrames);
  server.on("/update", []() { sendAsset(UPDATE_HTML_GZ, sizeof(UPDATE_HTML_GZ), "text/html; charset=utf-8", false); });
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); });
  server.begin();

  // Tarea CAN en el núcleo 1 (el 0 es de la Wi-Fi) con más prioridad que la web
  xTaskCreatePinnedToCore(canTask, "can", 6144, nullptr, 5, nullptr, 1);
  Serial.printf("PoweryMax Nmea2000 Diagnostic v%s listo. Wi-Fi %s  ->  http://%s\n", FW_VERSION, AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop() {
  server.handleClient();
  // Registro de estado por USB cada 5 s (115200 baudios): ayuda a diagnosticar la propia herramienta
  static uint32_t loops = 0, lastLog = 0;
  loops++;
  if (millis() - lastLog >= 5000) {
    lastLog = millis();
    Serial.printf("[estado] web=%u vueltas/5s peticiones=%u clientesWiFi=%d heap=%u can=%s tramas=%u\n",
      (unsigned)loops, (unsigned)httpReqs, WiFi.softAPgetStationNum(), (unsigned)ESP.getFreeHeap(), twState, (unsigned)core->bus.frames);
    loops = 0;
  }
  if (!otaValidated && millis() > 30000) {   // lleva 30 s funcionando: este firmware es bueno
    esp_ota_mark_app_valid_cancel_rollback();
    otaValidated = true;
  }
  delay(1);
}
