#include "n2k_core.h"
#include "fast_pgns.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

namespace n2k {

static const uint32_t PGN_ISO_REQUEST = 59904;
static const uint32_t PGN_ADDR_CLAIM = 60928;
static const uint32_t PGN_HEARTBEAT = 126993;
static const uint32_t PGN_PRODUCT_INFO = 126996;
static const uint32_t PGN_CONFIG_INFO = 126998;

Core::Core() {
  pg = new PgnEntry[MAX_PGN_ENTRIES];
  info = new Info[INFO_SLOTS];
  fp = new FpSlot[FP_SLOTS];
  reset(0);
}

void Core::reset(uint32_t now) {
  memset(dev, 0, sizeof(dev));
  for (int i = 0; i < 256; i++) dev[i].info = -1;
  infoUsed = 0;
  memset(pg, 0, sizeof(PgnEntry) * MAX_PGN_ENTRIES);
  memset(fp, 0, sizeof(FpSlot) * FP_SLOTS);
  memset(&bus, 0, sizeof(bus));
  pgUsed = 0; pgOverflow = 0; fpErrTotal = 0;
  startMs = now; bus.winStart = now;
  requestsSentAt = 0;
}

void Core::parseId(uint32_t id, uint32_t& pgn, uint8_t& sa, uint8_t& da, uint8_t& prio) {
  sa = id & 0xFF;
  uint8_t ps = (id >> 8) & 0xFF;
  uint8_t pf = (id >> 16) & 0xFF;
  uint32_t dp = (id >> 24) & 0x03;  // DP + EDP
  prio = (id >> 26) & 0x07;
  if (pf < 240) { pgn = (dp << 16) | ((uint32_t)pf << 8); da = ps; }
  else { pgn = (dp << 16) | ((uint32_t)pf << 8) | ps; da = 255; }
}

uint32_t Core::makeId(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa) {
  uint32_t pf = (pgn >> 8) & 0xFF;
  uint32_t id = ((uint32_t)(prio & 7) << 26) | ((pgn & 0x30000) << 8) | (pf << 16) | sa;
  if (pf < 240) id |= ((uint32_t)da << 8); else id |= ((pgn & 0xFF) << 8);
  return id;
}

bool Core::isFast(uint32_t pgn) {
  int lo = 0, hi = FAST_PGNS_N - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (FAST_PGNS[m] == pgn) return true;
    if (FAST_PGNS[m] < pgn) lo = m + 1; else hi = m - 1;
  }
  return false;
}

bool Core::isProprietary(uint32_t pgn) {
  return pgn == 61184 || (pgn >= 65280 && pgn <= 65535) || pgn == 126720 ||
         (pgn >= 130816 && pgn <= 131071);
}

uint8_t instanceOf(uint32_t pgn, const uint8_t* d, int len) {
  if (len < 2) return 0xFF;
  switch (pgn) {
    case 127488: case 127489: case 127493: case 127508: case 127245: case 127505:
      return d[0];
    case 127506: case 130312: case 130316: case 130314:
      return d[1];
    default: return 0xFF;
  }
}

static bool notAvailable(uint32_t pgn, const uint8_t* d, int len) {
  auto u16 = [&](int o) { return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8); };
  auto u32 = [&](int o) { return u16(o) | (u16(o + 2) << 16); };
  switch (pgn) {
    case 128267: return len >= 5 && u32(1) == 0xFFFFFFFFu;
    case 129025: return len >= 4 && u32(0) == 0x7FFFFFFFu;
    case 127250: return len >= 3 && u16(1) == 0xFFFF;
    case 129026: return len >= 6 && u16(4) == 0xFFFF;
    case 127508: return len >= 3 && u16(1) == 0x7FFF;
    case 127488: return len >= 3 && u16(1) == 0xFFFF;
    case 128259: return len >= 3 && u16(1) == 0xFFFF;
    default: return false;
  }
}

void Core::touchDevice(uint8_t sa, uint32_t now) {
  Device& d = dev[sa];
  if (!d.seen) { d.seen = true; d.firstSeen = now; d.lastSeen = now; }
  uint32_t gap = now - d.lastSeen;
  if (gap > d.maxGap) d.maxGap = gap;
  if (gap > 5000) d.gaps5s++;
  d.lastSeen = now;
  d.frames++;
}

Info* Core::infoFor(uint8_t sa) {
  Device& d = dev[sa];
  if (d.info < 0) {
    if (infoUsed >= INFO_SLOTS) return nullptr;
    d.info = infoUsed++;
    memset(&info[d.info], 0, sizeof(Info));
  }
  return &info[d.info];
}

PgnEntry* Core::entry(uint8_t sa, uint32_t pgn, uint8_t inst) {
  for (int i = 0; i < pgUsed; i++) {
    PgnEntry& e = pg[i];
    if (e.sa == sa && e.pgn == pgn && e.inst == inst) return &e;
  }
  if (pgUsed >= MAX_PGN_ENTRIES) { pgOverflow++; return nullptr; }
  PgnEntry& e = pg[pgUsed++];
  memset(&e, 0, sizeof(e));
  e.used = true; e.sa = sa; e.pgn = pgn; e.inst = inst; e.minI = 0xFFFFFFFF;
  return &e;
}

void Core::onFrame(const Frame& f, uint32_t now) {
  // Carga del bus: trama extendida = 67 bits + 8 por byte, + ~12 % de bit stuffing
  uint32_t bits = (67 + 8 * f.len) * 112 / 100;
  bus.frames++; bus.bits += bits; bus.winBits += bits;
  tick(now);

  uint32_t pgn; uint8_t sa, da, prio;
  parseId(f.id, pgn, sa, da, prio);
  if (sa == 254 && pgn != PGN_ADDR_CLAIM) { /* trama de un nodo sin dirección */ }
  touchDevice(sa, now);

  if (!isFast(pgn)) { onMessage(sa, da, pgn, f.data, f.len, now); return; }

  // ---- Reensamblado fast packet ----
  if (f.len < 2) return;
  uint8_t seq = f.data[0] >> 5, idx = f.data[0] & 0x1F;
  bool prop = isProprietary(pgn);
  FpSlot* s = nullptr; FpSlot* freeS = nullptr; FpSlot* oldest = nullptr;
  for (int i = 0; i < FP_SLOTS; i++) {
    FpSlot& c = fp[i];
    if (c.used && c.sa == sa && c.pgn == pgn) { s = &c; break; }
    if (!c.used && !freeS) freeS = &c;
    if (c.used && (!oldest || c.t < oldest->t)) oldest = &c;
  }
  if (idx == 0) {
    if (s && s->got < s->total && !prop) { dev[sa].fpErrors++; fpErrTotal++; }  // la anterior quedó incompleta
    if (!s) s = freeS ? freeS : oldest;
    if (!s) return;
    s->used = true; s->sa = sa; s->pgn = pgn; s->seq = seq; s->next = 1; s->t = now;
    s->total = f.data[1]; s->got = 0;
    for (int i = 2; i < f.len && s->got < s->total && s->got < sizeof(s->buf); i++) s->buf[s->got++] = f.data[i];
  } else {
    if (!s || s->seq != seq || s->next != idx) {
      if (!prop) { dev[sa].fpErrors++; fpErrTotal++; }
      if (s) s->used = false;
      return;
    }
    s->next++; s->t = now;
    for (int i = 1; i < f.len && s->got < s->total && s->got < sizeof(s->buf); i++) s->buf[s->got++] = f.data[i];
  }
  if (s->got >= s->total || s->got >= sizeof(s->buf)) {
    onMessage(sa, da, pgn, s->buf, s->got, now);
    s->used = false;
  }
}

void Core::tick(uint32_t now) {
  uint32_t dt = now - bus.winStart;
  if (dt >= 1000) {
    bus.loadNow = (float)bus.winBits * 100.0f * 1000.0f / (250000.0f * dt);
    if (bus.loadNow > bus.loadPeak) bus.loadPeak = bus.loadNow;
    bus.winBits = 0; bus.winStart = now;
  }
  // Limpia fast packets huérfanos (>750 ms sin continuación)
  for (int i = 0; i < FP_SLOTS; i++) {
    FpSlot& c = fp[i];
    if (c.used && now - c.t > 750) {
      if (!isProprietary(c.pgn)) { dev[c.sa].fpErrors++; fpErrTotal++; }
      c.used = false;
    }
  }
}

void Core::onMessage(uint8_t sa, uint8_t da, uint32_t pgn, const uint8_t* d, int len, uint32_t now) {
  Device& dv = dev[sa];
  if (pgn == PGN_ADDR_CLAIM && len >= 8) {
    uint64_t name = 0;
    for (int i = 7; i >= 0; i--) name = (name << 8) | d[i];
    dv.claims++;
    if (dv.frames > 1 && (requestsSentAt == 0 || now - requestsSentAt > 2000)) dv.claimsUnrequested++;
    dv.lastClaimMs = now;
    bool known = false;
    for (int i = 0; i < dv.nNames; i++) if (dv.names[i] == name) known = true;
    if (!known && dv.nNames < NAMES_MAX) dv.names[dv.nNames++] = name;
    dv.name = name;
    // Conflicto con nuestra dirección
    if (sa == ownAddr && ownName != 0 && name != ownName) {
      if (name < ownName) claimLost = true;  // gana el otro: cambiamos
      else answerClaimRequest = true;        // ganamos: reafirmamos
    }
  } else if (pgn == PGN_ISO_REQUEST && len >= 3) {
    uint32_t req = d[0] | (d[1] << 8) | ((uint32_t)d[2] << 16);
    if (req == PGN_ADDR_CLAIM && (da == 255 || da == ownAddr)) answerClaimRequest = true;
  } else if (pgn == PGN_PRODUCT_INFO) {
    Info* in = infoFor(sa);
    int n = len > PI_LEN ? PI_LEN : len;
    if (in) { memcpy(in->pi, d, n); in->piLen = n; }
  } else if (pgn == PGN_CONFIG_INFO) {
    Info* in = infoFor(sa);
    int n = len > CI_MAX ? CI_MAX : len;
    if (in) { memcpy(in->ci, d, n); in->ciLen = n; }
  } else if (pgn == PGN_HEARTBEAT && len >= 3) {
    uint8_t seq = d[2];
    if (dv.hbCount > 0) {
      dv.hbSumInt += now - dv.hbLast;
      // la secuencia vuelve a 0 sin venir de 252 → el equipo se ha reiniciado
      if (seq < dv.hbLastSeq && dv.hbLastSeq < 250 && seq != 0xFF) dv.hbResets++;
    }
    dv.hbCount++; dv.hbLastSeq = seq; dv.hbLast = now;
  }

  uint8_t inst = instanceOf(pgn, d, len);
  PgnEntry* e = entry(sa, pgn, inst);
  if (!e) return;
  if (e->n > 0) {
    uint32_t iv = now - e->last;
    if (iv < e->minI) e->minI = iv;
    if (iv > e->maxI) e->maxI = iv;
    e->sumI += iv;
  } else e->first = now;
  e->n++; e->last = now;
  int n = len > DATA_MAX ? DATA_MAX : len;
  memcpy(e->data, d, n); e->len = n;
  if (pgn == 127489 && len >= 24) e->orBits |= (uint32_t)d[20] | ((uint32_t)d[21] << 8) | ((uint32_t)d[22] << 16) | ((uint32_t)d[23] << 24);
  if (notAvailable(pgn, d, len)) e->naCount++;
}

// ---------------- JSON ----------------
struct Out {
  WriteFn w; void* ctx; char buf[512]; size_t n = 0;
  void flush() { if (n) { w(ctx, buf, n); n = 0; } }
  void put(const char* s) { while (*s) { if (n >= sizeof(buf)) flush(); buf[n++] = *s++; } }
  void f(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void hex(const uint8_t* d, int len) {
    static const char H[] = "0123456789abcdef";
    put("\"");
    for (int i = 0; i < len; i++) { if (n + 2 >= sizeof(buf)) flush(); buf[n++] = H[d[i] >> 4]; buf[n++] = H[d[i] & 15]; }
    put("\"");
  }
};
void Out::f(const char* fmt, ...) {
  char t[200]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof(t), fmt, a); va_end(a); put(t);
}

void Core::writeJson(WriteFn w, void* ctx, uint32_t now, const char* extraLive) {
  Out o; o.w = w; o.ctx = ctx;
  uint32_t dur = now - startMs;
  float loadAvg = dur > 0 ? (float)bus.bits * 100.0f * 1000.0f / (250000.0f * dur) : 0;
  o.f("{\"t\":%u,\"dur\":%u,\"bus\":{\"frames\":%u,\"loadNow\":%.1f,\"loadAvg\":%.1f,\"loadPeak\":%.1f,\"fpErr\":%u,\"pgOverflow\":%u,\"ownAddr\":%u}",
      (unsigned)now, (unsigned)dur, (unsigned)bus.frames, bus.loadNow, loadAvg, bus.loadPeak,
      (unsigned)fpErrTotal, (unsigned)pgOverflow, ownAddr);
  if (extraLive && *extraLive) { o.put(","); o.put(extraLive); }
  o.put(",\"devices\":[");
  bool first = true;
  for (int sa = 0; sa < 256; sa++) {
    Device& d = dev[sa];
    if (!d.seen) continue;
    if (!first) o.put(",");
    first = false;
    o.f("{\"sa\":%d,\"name\":\"%08x%08x\",\"names\":[", sa, (unsigned)(d.name >> 32), (unsigned)(d.name & 0xFFFFFFFF));
    for (int i = 0; i < d.nNames; i++) o.f("%s\"%08x%08x\"", i ? "," : "", (unsigned)(d.names[i] >> 32), (unsigned)(d.names[i] & 0xFFFFFFFF));
    o.f("],\"claims\":%u,\"claimsUnreq\":%u,\"first\":%u,\"last\":%u,\"frames\":%u,\"maxGap\":%u,\"gaps5s\":%u,\"fpErr\":%u,",
        d.claims, d.claimsUnrequested, (unsigned)(d.firstSeen - startMs), (unsigned)(d.lastSeen - startMs),
        (unsigned)d.frames, (unsigned)d.maxGap, d.gaps5s, d.fpErrors);
    o.f("\"hb\":{\"n\":%u,\"resets\":%u,\"avg\":%u},\"pi\":", d.hbCount, d.hbResets,
        (unsigned)(d.hbCount > 1 ? d.hbSumInt / (d.hbCount - 1) : 0));
    Info* in = d.info >= 0 ? &info[d.info] : nullptr;
    o.hex(in ? in->pi : nullptr, in ? in->piLen : 0);
    o.put(",\"ci\":");
    o.hex(in ? in->ci : nullptr, in ? in->ciLen : 0);
    o.put("}");
  }
  o.put("],\"pgns\":[");
  for (int i = 0; i < pgUsed; i++) {
    PgnEntry& e = pg[i];
    if (i) o.put(",");
    o.f("{\"sa\":%u,\"pgn\":%u,\"inst\":%u,\"n\":%u,\"first\":%u,\"last\":%u,\"minI\":%u,\"maxI\":%u,\"avgI\":%u,\"or\":%u,\"na\":%u,\"d\":",
        e.sa, (unsigned)e.pgn, e.inst, (unsigned)e.n, (unsigned)(e.first - startMs), (unsigned)(e.last - startMs),
        (unsigned)(e.n > 1 ? e.minI : 0), (unsigned)e.maxI, (unsigned)(e.n > 1 ? e.sumI / (e.n - 1) : 0),
        (unsigned)e.orBits, (unsigned)e.naCount);
    o.hex(e.data, e.len);
    o.put("}");
  }
  o.put("]}");
  o.flush();
}

}  // namespace n2k
