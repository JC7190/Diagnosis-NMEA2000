// Simulador de una red NMEA 2000 con averías conocidas para probar el núcleo.
// Compila: g++ -std=c++17 -I../firmware/n2kdiag sim.cpp ../firmware/n2kdiag/n2k_core.cpp -o sim
#include "n2k_core.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <functional>
using namespace n2k;

Core core;
uint32_t NOW = 0;

void sendRaw(uint8_t prio, uint32_t pgn, uint8_t sa, uint8_t da, const uint8_t* d, int len) {
  Frame f; f.id = Core::makeId(prio, pgn, da, sa); f.len = len; memcpy(f.data, d, len);
  core.onFrame(f, NOW);
}
uint8_t fpSeq[256];
void send(uint32_t pgn, uint8_t sa, const std::vector<uint8_t>& d, uint8_t da = 255, uint8_t prio = 3, int dropFrame = -1) {
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
uint64_t mkName(uint32_t uniq, uint16_t mfr, uint8_t fn, uint8_t cls, uint8_t inst = 0) {
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
void claim(uint8_t sa, uint64_t name) {
  std::vector<uint8_t> d(8);
  for (int i = 0; i < 8; i++) d[i] = (name >> (8 * i)) & 0xFF;
  send(60928, sa, d, 255, 6);
}
void str32(std::vector<uint8_t>& v, const char* s) {
  for (int i = 0; i < 32; i++) v.push_back(i < (int)strlen(s) ? s[i] : 0xFF);
}
void productInfo(uint8_t sa, uint16_t code, const char* model, const char* sw, const char* ver, const char* serial, uint8_t len) {
  std::vector<uint8_t> d;
  uint16_t v = 2100; d.push_back(v & 0xFF); d.push_back(v >> 8);
  d.push_back(code & 0xFF); d.push_back(code >> 8);
  str32(d, model); str32(d, sw); str32(d, ver); str32(d, serial);
  d.push_back(1); d.push_back(len);
  send(126996, sa, d, 255, 6);
}
void le16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
void le32(std::vector<uint8_t>& v, uint32_t x) { le16(v, x & 0xFFFF); le16(v, x >> 16); }

uint8_t hbSeq[256];
void heartbeat(uint8_t sa) {
  std::vector<uint8_t> d; le16(d, 60000); d.push_back(hbSeq[sa]++); d.push_back(0xFF);
  for (int i = 0; i < 4; i++) d.push_back(0xFF);
  send(126993, sa, d, 255, 7);
}

struct SimDev { uint8_t sa; uint64_t name; uint16_t code; const char *model, *sw, *ver, *serial; uint8_t len; };

int main() {
  core.ownAddr = 251;
  core.ownName = mkName(12345, 2046, 130, 10);
  core.reset(0);
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
  claim(40, mkName(2001, 140, 130, 120));   // dos equipos con la misma dirección
  claim(254, mkName(3001, 229, 130, 120));  // equipo que no consigue dirección

  for (NOW = 0; NOW <= 60000; NOW += 10) {
    bool ghostDown = NOW >= 30000 && NOW < 37000;
    // Petición activa en t=1 s
    if (NOW == 1000) {
      core.requestsSentAt = NOW;
      for (auto& d : devs) {
        if (d.sa == 3 && ghostDown) continue;
        claim(d.sa, d.name);
        if (d.sa != 30) productInfo(d.sa, d.code, d.model, d.sw, d.ver, d.serial, d.len);  // el 30 no contesta
      }
    }
    if (NOW == 1500) claim(40, mkName(2002, 1857, 130, 120));
    if (NOW == 37000) { claim(3, devs[1].name); hbSeq[3] = 0; }  // Ghost vuelve tras reiniciarse
    if (NOW % 10000 == 10) for (auto& d : devs) if (!(d.sa == 3 && ghostDown)) heartbeat(d.sa);

    if (NOW % 100 == 0) {
      // GPS 10 Hz
      std::vector<uint8_t> p; le32(p, (uint32_t)(36.5933 * 1e7)); le32(p, (uint32_t)(int32_t)(-6.2297 * 1e7));
      send(129025, 5, p, 255, 2);
      std::vector<uint8_t> h = {0}; le16(h, 15000); le16(h, 0x7FFF); le16(h, 0x7FFF); h.push_back(0xFD);
      send(127250, 5, h, 255, 2);
      // Motores 10 Hz — los dos con instancia 0 (avería de configuración)
      for (uint8_t sa : {10, 11}) {
        std::vector<uint8_t> e = {0}; le16(e, 4 * (sa == 10 ? 750 : 760)); le16(e, 0xFFFF); e.push_back(10); le16(e, 0xFFFF);
        send(127488, sa, e, 255, 2);
      }
      if (!ghostDown) { std::vector<uint8_t> g = {0x41, 0x9f, 1, 2, 3, 4, 5, 6}; send(130824 - 0 + 1, 3, g); }  // 130825 propietario
    }
    if (NOW % 250 == 0) {
      std::vector<uint8_t> c = {0, 0xFC}; le16(c, 10000); le16(c, 250); le16(c, 0xFFFF);
      send(129026, 5, c, 255, 2);
    }
    if (NOW % 500 == 0) {
      for (uint8_t sa : {10, 11}) {
        std::vector<uint8_t> e = {0}; le16(e, 3000); le16(e, 0xFFFF); le16(e, (uint32_t)((273.15 + 72) * 100)); le16(e, 1410);
        le16(e, 120); le32(e, 3600 * 412); le16(e, 0xFFFF); le16(e, 0xFFFF); e.push_back(0xFF);
        uint16_t st1 = (sa == 11 && NOW > 20000) ? (1 << 2) : 0;  // presión de aceite baja en el 2º motor
        le16(e, st1); le16(e, 0); e.push_back(40); e.push_back(0x7F);
        // en t=45 s se pierde un trozo del fast packet del motor 2
        send(127489, sa, e, 255, 2, (sa == 11 && NOW == 45000) ? 2 : -1);
      }
    }
    if (NOW % 1000 == 0) {
      std::vector<uint8_t> g = {0}; le16(g, 20412); le32(g, 360000000);
      for (int i = 0; i < 8; i++) g.push_back(0); for (int i = 0; i < 8; i++) g.push_back(0); for (int i = 0; i < 8; i++) g.push_back(0);
      g.push_back(0x13); g.push_back(0xFC); g.push_back(9); le16(g, 90); le16(g, 160); le32(g, 0); g.push_back(0);
      send(129029, 5, g, 255, 3);
      std::vector<uint8_t> dp = {0}; le32(dp, 0xFFFFFFFF); le16(dp, 0); dp.push_back(0xFF);  // sonda fuera del agua
      send(128267, 20, dp, 255, 3);
      std::vector<uint8_t> sp = {0}; le16(sp, 0); le16(sp, 0xFFFF); sp.push_back(0); sp.push_back(0xFF); sp.push_back(0xFF);
      send(128259, 20, sp, 255, 2);
    }
    if (NOW % 2000 == 0) {
      std::vector<uint8_t> t = {0, 0, 0}; t.push_back(0); le16(t, 0); le16(t, 0xFFFF);
      // 130316: temp extended 24 bit
      std::vector<uint8_t> te = {0, 0, 0}; uint32_t k = (uint32_t)((273.15 + 18.4) * 1000); te.push_back(k & 0xFF); te.push_back((k >> 8) & 0xFF); te.push_back(k >> 16); le16(te, 0xFFFF);
      send(130316, 20, te, 255, 5);
    }
    if (NOW % 1500 == 0) {
      std::vector<uint8_t> b = {0}; le16(b, 1140); le16(b, (uint32_t)(int16_t)-85 & 0xFFFF); le16(b, 0xFFFF); b.push_back(0);
      send(127508, 30, b, 255, 6);
    }
    core.tick(NOW);
  }
  std::string out;
  const char* live = "\"live\":{\"v\":{\"now\":11.62,\"min\":10.18,\"max\":12.71,\"avg\":11.9,\"ok\":1},"
                     "\"twai\":{\"state\":\"running\",\"tec\":0,\"rec\":12,\"busErr\":61,\"missed\":0,\"overrun\":0,\"arbLost\":3,\"txFailed\":0},"
                     "\"alerts\":{\"busOff\":0,\"errPass\":1,\"recovered\":0},\"mode\":\"active\"}";
  core.writeJson([](void* c, const char* s, size_t n) { ((std::string*)c)->append(s, n); }, &out, NOW, live);
  fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}
