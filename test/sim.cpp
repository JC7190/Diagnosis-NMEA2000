// Simulador de redes NMEA 2000 para probar el núcleo (n2k_core) sin hardware.
// Compila: g++ -std=c++17 -O2 -I../firmware/n2kdiag sim.cpp ../firmware/n2kdiag/n2k_core.cpp -o sim
// Uso:     ./sim [averias|limpia|grande] [lite] > snapshot.json
//   averias  red con averías conocidas (cada una debe detectarse)
//   limpia   red sana con situaciones que antes daban falsos positivos
//   grande   red grande y ruidosa (120 equipos, fast packets intercalados, tramas perdidas)
#include "n2k_core.h"
#include <string>
#include <vector>
#include <deque>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
using namespace n2k;

static Core* core;
static uint32_t NOW = 0;
static uint32_t rng = 12345;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static double dropProb = 0;     // probabilidad de perder una trama (ruido)
static uint64_t framesSent = 0, framesDropped = 0;

// Cola de tramas por equipo: en "grande" se intercalan entre equipos como en un bus real
static bool interleave = false;
static std::deque<Frame> q[256];

static void deliver(const Frame& f) {
  framesSent++;
  if (dropProb > 0 && (rnd() % 1000000) < dropProb * 1000000) { framesDropped++; return; }
  core->onFrame(f, NOW);
}
static void flushQueues() {
  for (;;) {
    int live[256], n = 0;
    for (int i = 0; i < 256; i++) if (!q[i].empty()) live[n++] = i;
    if (!n) return;
    int s = live[rnd() % n];
    deliver(q[s].front()); q[s].pop_front();
  }
}
static void sendRaw(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da, const uint8_t* d, int len) {
  Frame f; f.id = Core::makeId(prio, pgn, da, sa); f.len = len; memcpy(f.data, d, len);
  if (interleave) q[sa].push_back(f); else deliver(f);
}
static uint8_t fpSeq[256];
static void send(uint32_t pgn, uint8_t sa, const std::vector<uint8_t>& d, uint8_t da = 255, uint8_t prio = 3, int dropFrame = -1) {
  if (!Core::isFast(pgn)) { sendRaw(prio, pgn, sa, da, d.data(), (int)d.size()); return; }
  uint8_t seq = (fpSeq[sa]++) & 7;
  uint8_t f[8]; int idx = 0; size_t p = 0;
  f[0] = (seq << 5) | idx; f[1] = (uint8_t)d.size();
  for (int i = 2; i < 8; i++) f[i] = p < d.size() ? d[p++] : 0xFF;
  sendRaw(prio, pgn, sa, da, f, 8);
  while (p < d.size()) {
    idx++; f[0] = (seq << 5) | idx;
    for (int i = 1; i < 8; i++) f[i] = p < d.size() ? d[p++] : 0xFF;
    if (idx != dropFrame) sendRaw(prio, pgn, sa, da, f, 8);
  }
}
static uint64_t mkName(uint32_t uniq, uint16_t mfr, uint8_t fn, uint8_t cls, uint8_t inst = 0) {
  uint64_t n = 0;
  n |= (uint64_t)(uniq & 0x1FFFFF);
  n |= (uint64_t)(mfr & 0x7FF) << 21;
  n |= (uint64_t)inst << 32;
  n |= (uint64_t)fn << 40;
  n |= (uint64_t)(cls & 0x7F) << 49;
  n |= (uint64_t)4 << 60;  // marine
  n |= (uint64_t)1 << 63;
  return n;
}
static void claim(uint8_t sa, uint64_t name) {
  std::vector<uint8_t> d(8);
  for (int i = 0; i < 8; i++) d[i] = (name >> (8 * i)) & 0xFF;
  send(60928, sa, d, 255, 6);
}
static void isoRequest(uint8_t sa, uint32_t pgn, uint8_t da = 255) {
  std::vector<uint8_t> d = { (uint8_t)(pgn & 0xFF), (uint8_t)(pgn >> 8), (uint8_t)(pgn >> 16) };
  send(59904, sa, d, da, 6);
}
static void str32(std::vector<uint8_t>& v, const char* s) {
  for (int i = 0; i < 32; i++) v.push_back(i < (int)strlen(s) ? s[i] : 0xFF);
}
static void productInfo(uint8_t sa, uint16_t code, const char* model, const char* sw, const char* ver, const char* serial, uint8_t len) {
  std::vector<uint8_t> d;
  uint16_t v = 2100; d.push_back(v & 0xFF); d.push_back(v >> 8);
  d.push_back(code & 0xFF); d.push_back(code >> 8);
  str32(d, model); str32(d, sw); str32(d, ver); str32(d, serial);
  d.push_back(1); d.push_back(len);
  send(126996, sa, d, 255, 6);
}
static void le16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
static void le32(std::vector<uint8_t>& v, uint32_t x) { le16(v, x & 0xFFFF); le16(v, x >> 16); }

static uint8_t hbSeq[256];
static void heartbeat(uint8_t sa) {
  std::vector<uint8_t> d; le16(d, 60000); d.push_back(hbSeq[sa]); d.push_back(0xFF);
  hbSeq[sa] = hbSeq[sa] >= 252 ? 0 : hbSeq[sa] + 1;
  for (int i = 0; i < 4; i++) d.push_back(0xFF);
  send(126993, sa, d, 255, 7);
}
static void engineRapid(uint8_t sa, uint8_t inst, uint32_t rpm) {
  std::vector<uint8_t> e = {inst}; le16(e, rpm == 0xFFFF ? 0xFFFF : 4 * rpm); le16(e, 0xFFFF); e.push_back(10); le16(e, 0xFFFF);
  send(127488, sa, e, 255, 2);
}
static void engineDyn(uint8_t sa, uint8_t inst, uint16_t st1, uint16_t st2, int dropFrame = -1) {
  std::vector<uint8_t> e = {inst}; le16(e, 3000); le16(e, 0xFFFF); le16(e, (uint32_t)((273.15 + 72) * 100)); le16(e, 1410);
  le16(e, 120); le32(e, 3600 * 412); le16(e, 0xFFFF); le16(e, 0xFFFF); e.push_back(0xFF);
  le16(e, st1); le16(e, st2); e.push_back(40); e.push_back(0x7F);
  send(127489, sa, e, 255, 2, dropFrame);
}
static void battery(uint8_t sa, uint8_t inst, double volts) {
  std::vector<uint8_t> b = {inst}; le16(b, (uint32_t)(int)(volts * 100)); le16(b, (uint32_t)(int16_t)-85 & 0xFFFF); le16(b, 0xFFFF); b.push_back(0);
  send(127508, sa, b, 255, 6);
}
static void temperature(uint8_t sa, uint8_t inst, uint8_t src, double c) {
  std::vector<uint8_t> t = {0, inst, src}; le16(t, (uint32_t)((273.15 + c) * 100)); le16(t, 0xFFFF); t.push_back(0xFF);
  send(130312, sa, t, 255, 5);
}
static void gnss(uint8_t sa, uint8_t method, uint8_t svs, uint16_t hdop) {
  std::vector<uint8_t> g = {0}; le16(g, 20412); le32(g, 360000000);
  for (int i = 0; i < 24; i++) g.push_back(0);
  g.push_back((uint8_t)(0x03 | (method << 4))); g.push_back(0xFC); g.push_back(svs); le16(g, hdop); le16(g, 160); le32(g, 0); g.push_back(0);
  send(129029, sa, g, 255, 3);
}
static void position(uint8_t sa, bool valid) {
  std::vector<uint8_t> p;
  if (valid) { le32(p, (uint32_t)(36.5933 * 1e7)); le32(p, (uint32_t)(int32_t)(-6.2297 * 1e7)); }
  else { le32(p, 0x7FFFFFFF); le32(p, 0x7FFFFFFF); }
  send(129025, sa, p, 255, 2);
}
static void depth(uint8_t sa, bool valid) {
  std::vector<uint8_t> dp = {0}; le32(dp, valid ? 1234 : 0xFFFFFFFF); le16(dp, 0); dp.push_back(0xFF);
  send(128267, sa, dp, 255, 3);
}
static void rudder(uint8_t sa, uint8_t inst, bool positionValid) {
  std::vector<uint8_t> r = {inst, 0xF8}; le16(r, positionValid ? 0x7FFF : 120); le16(r, positionValid ? 50 : 0x7FFF); le16(r, 0xFFFF);
  send(127245, sa, r, 255, 2);
}

struct SimDev { uint8_t sa; uint64_t name; uint16_t code; const char *model, *sw, *ver, *serial; uint8_t len; };

static const char* LIVE_OK =
  "\"live\":{\"fw\":\"sim\",\"v\":{\"now\":12.62,\"min\":12.41,\"max\":12.83,\"avg\":12.6,\"dev\":0.12,\"ok\":1},"
  "\"twai\":{\"state\":\"funcionando\",\"tec\":0,\"rec\":0,\"busErr\":0,\"missed\":0,\"overrun\":0,\"arbLost\":0,\"txFailed\":0},"
  "\"alerts\":{\"busOff\":0,\"errPass\":0,\"recovered\":0},\"mode\":\"active\"}";

// ------------------------------------------------------------------ averías
static const char* scenarioAverias() {
  std::vector<SimDev> devs = {
    {0, mkName(1001, 140, 130, 120), 19110, "HDS LIVE 12", "23.1.0.146", "1", "105301234", 2},
    {3, mkName(1002, 140, 150, 40), 19900, "Ghost 52in", "1.8.0.11", "A", "203456789", 1},
    {5, mkName(1003, 140, 145, 60), 15160, "Point-1", "2.1.65", "1", "102938475", 1},
    {10, mkName(1004, 1862, 160, 50), 6123, "Yamaha Gateway", "1.2.0", "1", "YG00011", 1},
    {11, mkName(1005, 1862, 160, 50, 1), 6123, "Yamaha Gateway", "1.2.0", "1", "YG00012", 1},
    {20, mkName(1006, 135, 136, 60), 3140, "DST810", "1.006", "1", "2349881", 1},
    {30, mkName(1007, 358, 170, 35), 2501, "BMV-712", "4.08", "1", "HQ2033", 2},
  };
  for (auto& d : devs) claim(d.sa, d.name);
  claim(254, mkName(3001, 229, 130, 120));  // equipo que no consigue dirección

  for (NOW = 0; NOW <= 60000; NOW += 10) {
    bool ghostDown = NOW >= 30000 && NOW < 37000;
    if (NOW == 1000) {   // petición activa
      core->requestsSentAt = NOW;
      for (auto& d : devs) {
        if (d.sa == 3 && ghostDown) continue;
        claim(d.sa, d.name);
        if (d.sa != 30) productInfo(d.sa, d.code, d.model, d.sw, d.ver, d.serial, d.len);  // el 30 no contesta
      }
    }
    // dos pantallas que se pelean por la dirección 40 durante toda la prueba
    if (NOW % 8000 == 1500) claim(40, mkName(2001, 140, 130, 120));
    if (NOW % 8000 == 5500) claim(40, mkName(2002, 1857, 130, 120));
    if (NOW == 37000) { claim(3, devs[1].name); hbSeq[3] = 0; }  // Ghost vuelve tras reiniciarse
    if (NOW % 10000 == 10) for (auto& d : devs) if (!(d.sa == 3 && ghostDown)) heartbeat(d.sa);

    if (NOW % 100 == 0) {
      position(5, true);
      std::vector<uint8_t> h = {0}; le16(h, 15000); le16(h, 0x7FFF); le16(h, 0x7FFF); h.push_back(0xFD);
      send(127250, 5, h, 255, 2);
      // Motores 10 Hz — los dos con instancia 0 (avería de configuración), en marcha
      engineRapid(10, 0, 750); engineRapid(11, 0, 760);
      if (!ghostDown) { std::vector<uint8_t> g = {0x41, 0x9f, 1, 2, 3, 4, 5, 6}; send(130825, 3, g); }  // propietario
    }
    if (NOW % 250 == 0) {
      std::vector<uint8_t> c = {0, 0xFC}; le16(c, 10000); le16(c, 250); le16(c, 0xFFFF);
      send(129026, 5, c, 255, 2);
    }
    if (NOW % 500 == 0) {
      for (uint8_t sa : {10, 11}) {
        uint16_t st1 = (sa == 11 && NOW > 20000) ? (1 << 2) : 0;  // presión de aceite baja en marcha
        int drop = (sa == 11 && (NOW == 45000 || NOW == 46000 || NOW == 47000)) ? 2 : -1;   // 3 mensajes cortados
        engineDyn(sa, 0, st1, 0, drop);
      }
    }
    if (NOW % 1000 == 0) {
      gnss(5, 1, 9, 90);
      depth(20, false);   // sonda siempre vacía
      std::vector<uint8_t> sp = {0}; le16(sp, 0); le16(sp, 0xFFFF); sp.push_back(0); sp.push_back(0xFF); sp.push_back(0xFF);
      send(128259, 20, sp, 255, 2);
    }
    if (NOW % 1500 == 0) battery(30, 0, 11.40);
    core->tick(NOW);
  }
  return "\"live\":{\"fw\":\"sim\",\"v\":{\"now\":11.62,\"min\":10.18,\"max\":12.71,\"avg\":11.9,\"dev\":1.6,\"ok\":1},"
         "\"twai\":{\"state\":\"funcionando\",\"tec\":0,\"rec\":12,\"busErr\":61,\"missed\":0,\"overrun\":0,\"arbLost\":3,\"txFailed\":0},"
         "\"alerts\":{\"busOff\":0,\"errPass\":1,\"recovered\":0},\"mode\":\"active\"}";
}

// ------------------------------------------------------------------ limpia
static const char* scenarioLimpia() {
  std::vector<SimDev> devs = {
    {0, mkName(1001, 140, 130, 120), 19110, "HDS LIVE 12", "23.1.0.146", "1", "105301234", 2},
    {3, mkName(1002, 140, 150, 40), 19900, "Ghost 52in", "1.8.0.11", "A", "203456789", 1},
    {5, mkName(1003, 140, 145, 60), 15160, "Point-1", "2.1.65", "1", "102938475", 1},
    {10, mkName(1004, 1862, 160, 50), 6123, "Yamaha Gateway", "1.2.0", "1", "YG00011", 0xFF},  // LEN no disponible
    {20, mkName(1006, 135, 136, 60), 3140, "DST810", "1.006", "1", "2349881", 1},
    {21, mkName(1008, 137, 130, 75), 4100, "TMP100", "1.0", "1", "T1", 1},
    {22, mkName(1009, 137, 130, 75), 4100, "TMP100", "1.0", "1", "T2", 1},
    {40, mkName(1010, 275, 150, 40), 5000, "NAC-2", "2.0", "1", "AP1", 1},
    {41, mkName(1011, 275, 155, 40), 5001, "RF25N", "1.0", "1", "RF1", 1},
    {50, mkName(1012, 135, 195, 60), 6000, "AIS-650", "1.0", "1", "AIS1", 1},
  };
  // Empieza a escuchar a mitad de un fast packet (trozos sueltos: no es avería)
  { uint8_t f[8] = {0x23, 1, 2, 3, 4, 5, 6, 7}; sendRaw(3, 129029, 5, 255, f, 8); f[0] = 0x24; sendRaw(3, 129029, 5, 255, f, 8); }
  for (auto& d : devs) claim(d.sa, d.name);
  hbSeq[0] = 249;   // la secuencia del heartbeat da la vuelta (252 → 0): no es reinicio
  uint64_t movName = mkName(1013, 358, 170, 35);
  uint64_t newcomer = mkName(1500, 1857, 130, 120);

  for (NOW = 0; NOW <= 120000; NOW += 10) {
    if (NOW == 1000) {
      core->requestsSentAt = NOW;
      for (auto& d : devs) { claim(d.sa, d.name); productInfo(d.sa, d.code, d.model, d.sw, d.ver, d.serial, d.len); }
    }
    // Una pantalla pide las direcciones a todos a los 20 s: todos contestan (no son reinicios)
    if (NOW == 20000) { isoRequest(0, 60928); }
    if (NOW == 20030) for (auto& d : devs) claim(d.sa, d.name);
    // Un equipo nuevo se enciende y reclama la dirección del GPS; el GPS (NAME menor) la
    // defiende y el nuevo se va a la 6: reparto normal de direcciones
    if (NOW == 30000) claim(5, newcomer);
    if (NOW == 30010) claim(5, devs[2].name);
    if (NOW == 30020) { claim(6, newcomer); productInfo(6, 7000, "Zeus3", "1.0", "1", "Z1", 3); }
    if (NOW >= 30020 && NOW % 1000 == 20) { std::vector<uint8_t> t = {0}; le32(t, 0); le16(t, 0); send(126992, 6, t); }
    // Monitor de batería que cambia de dirección (mismo equipo) a los 60 s
    uint8_t movSa = NOW < 60000 ? 30 : 31;
    if (NOW == 0 || NOW == 60000) { claim(movSa, movName); productInfo(movSa, 2501, "BMV-712", "4.08", "1", "HQ2033", 2); }
    if (NOW % 1500 == 0) battery(movSa, 0, 12.7);

    if (NOW % 60000 == 10) for (auto& d : devs) heartbeat(d.sa);
    if (NOW % 100 == 0) {
      // Motor PARADO con el contacto dado: presión de aceite y carga activadas (normal)
      engineRapid(10, 0, 0);
      if (NOW % 500 == 0) engineDyn(10, 0, (1 << 2) | (1 << 9), 0xFFFF);
      // Piloto: orden de timón (posición vacía) y sensor de timón: no es duplicado
      rudder(40, 0, false); rudder(41, 0, true);
      std::vector<uint8_t> g = {0x41, 0x9f, 1, 2, 3, 4, 5, 6}; send(130825, 3, g);
    }
    if (NOW % 1000 == 0) {
      position(5, true);   // 1 Hz (la norma dice 10 Hz): solo informativo
      gnss(5, 1, 0xFF, 0x7FFF);   // satélites y HDOP "no disponible": no es señal pobre
      depth(20, true);
      temperature(21, 0, 0, 18.5);   // agua de mar, instancia 0
      temperature(22, 0, 3, 41.0);   // sala de máquinas, instancia 0 → no es duplicado
      battery(3, 1, 36.6);           // batería del motor eléctrico (36 V): no es "tensión alta"
    }
    // AIS: ráfagas cada 6 s (no es un equipo que se desconecta)
    if (NOW % 6000 == 0) for (int k = 0; k < 6; k++) {
      std::vector<uint8_t> a(27, 0x11); send(129038, 50, a, 255, 4);
    }
    core->tick(NOW);
  }
  return LIVE_OK;
}

// ------------------------------------------------------------------ grande
static const char* scenarioGrande() {
  interleave = true;
  dropProb = 0.0005;   // 1 de cada 2000 tramas se pierde
  const int N = 120;
  std::vector<SimDev> devs;
  static char model[N][16], serial[N][16];
  for (int i = 0; i < N; i++) {
    snprintf(model[i], 16, "Equipo %d", i); snprintf(serial[i], 16, "SN%05d", i);
    devs.push_back({(uint8_t)(i * 2), mkName(5000 + i, 140 + (i % 5), 130, 120), (uint16_t)(1000 + i), model[i], "1.0", "1", serial[i], 1});
  }
  for (auto& d : devs) claim(d.sa, d.name);
  flushQueues();
  for (NOW = 0; NOW <= 60000; NOW += 10) {
    // como el firmware en redes grandes: peticiones dirigidas, una cada 20 ms
    if (NOW >= 1000 && NOW < 1000 + N * 20 && (NOW - 1000) % 20 == 0) {
      auto& d = devs[(NOW - 1000) / 20];
      productInfo(d.sa, d.code, d.model, d.sw, d.ver, d.serial, d.len);
    }
    // ráfaga: 60 equipos mandan a la vez un mensaje largo (fast packets intercalados)
    if (NOW % 15000 == 5000) for (int i = 0; i < 60; i++) gnss(devs[i].sa, 1, 10, 90);
    for (int i = 0; i < N; i++) {
      uint8_t sa = devs[i].sa;
      if ((NOW + i * 10) % 1000 == 0) { gnss(sa, 1, 10, 90); std::vector<uint8_t> t = {0}; le32(t, 0); le16(t, 0); send(126992, sa, t); }
      if ((NOW + i * 10) % 500 == 0) { std::vector<uint8_t> w(8, 0x22); send(130306 + (i % 4), sa, w); }
      if (i < 20 && NOW % 100 == 0) { std::vector<uint8_t> x(8, 0x33); send(127250, sa, x, 255, 2); }
      // muchos PGN distintos para llenar la tabla (desbordamiento controlado)
      if (NOW % 2000 == (uint32_t)(i % 200) * 10) { std::vector<uint8_t> y(8, 1); send(65280 + (i % 40), sa, y, 255, 7); }
    }
    if (NOW % 10000 == 10) for (auto& d : devs) heartbeat(d.sa);
    flushQueues();
    core->tick(NOW);
  }
  return "\"live\":{\"fw\":\"sim\",\"v\":{\"now\":12.62,\"min\":12.41,\"max\":12.83,\"avg\":12.6,\"dev\":0.12,\"ok\":1},"
         "\"twai\":{\"state\":\"funcionando\",\"tec\":0,\"rec\":0,\"busErr\":0,\"missed\":0,\"overrun\":0,\"arbLost\":0,\"txFailed\":0},"
         "\"alerts\":{\"busOff\":0,\"errPass\":0,\"recovered\":0},\"mode\":\"active\"}";
}

int main(int argc, char** argv) {
  std::string sc = argc > 1 ? argv[1] : "averias";
  bool lite = argc > 2 && std::string(argv[2]) == "lite";
  core = new Core();
  core->ownAddr = 251;
  core->ownName = mkName(12345, 2046, 130, 10);
  core->reset(0);
  auto t0 = std::chrono::steady_clock::now();
  const char* live = sc == "limpia" ? scenarioLimpia() : sc == "grande" ? scenarioGrande() : scenarioAverias();
  auto t1 = std::chrono::steady_clock::now();
  std::string out;
  int locks = 0;
  core->writeJson([](void* c, const char* s, size_t n) { ((std::string*)c)->append(s, n); }, &out, NOW, live,
                  [](void* c, bool on) { if (on) ++*(int*)c; }, &locks, lite);
  auto t2 = std::chrono::steady_clock::now();
  fwrite(out.data(), 1, out.size(), stdout);
  fprintf(stderr, "[%s] tramas %llu (perdidas %llu)  procesado %.1f ms  JSON %zu bytes en %.2f ms (%d bloqueos cortos)\n",
          sc.c_str(), (unsigned long long)framesSent, (unsigned long long)framesDropped,
          std::chrono::duration<double, std::milli>(t1 - t0).count(), out.size(),
          std::chrono::duration<double, std::milli>(t2 - t1).count(), locks);
  return 0;
}
