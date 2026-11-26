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
static const uint32_t PGN_ENGINE_RAPID = 127488;
static const uint32_t PGN_ENGINE_DYN = 127489;

Core::Core() {
  pg = new PgnEntry[MAX_PGN_ENTRIES];
  pgHead = new int16_t[PG_HASH];
  pdata = new uint8_t[DATA_SLOTS][DATA_MAX];
  plen = new uint8_t[DATA_SLOTS];
  info = new Info[INFO_SLOTS];
  fp = new FpSlot[FP_SLOTS];
  reset(0);
}

void Core::reset(uint32_t now) {
  memset(dev, 0, sizeof(dev));
  for (int i = 0; i < 256; i++) dev[i].info = -1;
  infoUsed = 0; infoOverflow = 0;
  memset(pg, 0, sizeof(PgnEntry) * MAX_PGN_ENTRIES);
  for (int i = 0; i < PG_HASH; i++) pgHead[i] = -1;
  memset(fp, 0, sizeof(FpSlot) * FP_SLOTS);
  memset(&bus, 0, sizeof(bus));
  memset(engRpm, 0xFF, sizeof(engRpm)); memset(engRpmMs, 0, sizeof(engRpmMs));
  pgUsed = 0; pgOverflow = 0; fpErrTotal = 0; dataUsed = 0;
  startMs = now; bus.winStart = now;
  requestsSentAt = 0; claimReqMs = 0; claimReqSeen = false;
  claimLost = false; answerClaimRequest = false;
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

// Instancia del dato. En temperatura, humedad y presión la "fuente" (mar, motor,
// cabina…) forma parte de la identidad: dos sensores con la misma instancia y
// distinta fuente NO están duplicados.
uint16_t instanceOf(uint32_t pgn, const uint8_t* d, int len) {
  if (len < 2) return 0xFF;
  switch (pgn) {
    case 127488: case 127489: case 127493: case 127497: case 127508: case 127245:
    case 127505: case 127501:
      return d[0];
    case 127506: case 127751:
      return d[1];
    case 130312: case 130313: case 130314: case 130315: case 130316:
      return len >= 3 ? (uint16_t)(d[1] | ((uint16_t)d[2] << 8)) : d[1];
    case 130306:   // viento: la "instancia" es la referencia (aparente, verdadero…)
      return len >= 6 ? (d[5] & 7) : 0xFF;
    default: return 0xFF;
  }
}

// Solo se guarda el contenido de los mensajes que analyze.js descodifica
bool keepsData(uint32_t pgn) {
  return pgn == 127245 || pgn == 127508 || pgn == 129029;
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
  d.lastGap = gap;
  if (gap > d.maxGap) d.maxGap = gap;
  if (gap > 5000) d.gaps5s++;
  d.lastSeen = now;
  d.frames++;
}

Info* Core::infoFor(uint8_t sa) {
  Device& d = dev[sa];
  if (d.info < 0) {
    if (infoUsed >= INFO_SLOTS) { infoOverflow++; return nullptr; }
    d.info = infoUsed++;
    memset(&info[d.info], 0, sizeof(Info));
  }
  return &info[d.info];
}

static inline uint32_t pgHash(uint8_t sa, uint32_t pgn, uint16_t inst) {
  uint32_t h = pgn * 2654435761u ^ ((uint32_t)sa * 40503u) ^ ((uint32_t)inst * 97u);
  return (h ^ (h >> 15)) & (PG_HASH - 1);
}

PgnEntry* Core::entry(uint8_t sa, uint32_t pgn, uint16_t inst) {
  uint32_t h = pgHash(sa, pgn, inst);
  for (int16_t i = pgHead[h]; i >= 0; i = pg[i].next) {
    PgnEntry& e = pg[i];
    if (e.sa == sa && e.pgn == pgn && e.inst == inst) return &e;
  }
  if (pgUsed >= MAX_PGN_ENTRIES) { pgOverflow++; return nullptr; }
  int idx = pgUsed++;
  PgnEntry& e = pg[idx];
  memset(&e, 0, sizeof(e));
  e.sa = sa; e.pgn = pgn; e.inst = inst; e.minI = 0xFFFFFFFF;
  e.data = -1;
  if (keepsData(pgn) && dataUsed < DATA_SLOTS) { e.data = (int8_t)dataUsed; plen[dataUsed++] = 0; }
  e.next = pgHead[h]; pgHead[h] = (int16_t)idx;
  return &e;
}

void Core::fpError(uint8_t sa, uint32_t pgn) {
  if (isProprietary(pgn)) return;   // hay propietarios de una sola trama con el mismo PGN
  dev[sa].fpErrors++; fpErrTotal++;
}

void Core::onFrame(const Frame& f, uint32_t now) {
  // Carga del bus: trama extendida = 67 bits + 8 por byte, + ~12 % de bit stuffing
  uint32_t bits = (67 + 8 * f.len) * 112 / 100;
  bus.frames++; bus.bits += bits; bus.winBits += bits;
  tick(now);

  uint32_t pgn; uint8_t sa, da, prio;
  parseId(f.id, pgn, sa, da, prio);
  if (sa == 255) return;   // 255 no es una dirección de origen válida
  touchDevice(sa, now);

  if (!isFast(pgn)) { onMessage(sa, da, pgn, f.data, f.len, now); return; }

  // ---- Reensamblado fast packet ----
  // Clave: (origen, PGN). Un mensaje roto se cuenta UNA vez y el resto de sus
  // trozos se descartan en silencio hasta que empiece el siguiente.
  if (f.len < 2) return;
  uint8_t seq = f.data[0] >> 5, idx = f.data[0] & 0x1F;
  FpSlot* s = nullptr; FpSlot* freeS = nullptr; FpSlot* oldest = nullptr;
  for (int i = 0; i < FP_SLOTS; i++) {
    FpSlot& c = fp[i];
    if (c.used) {
      if (c.sa == sa && c.pgn == pgn) { s = &c; break; }
      if (!oldest || (int32_t)(c.t - oldest->t) < 0) oldest = &c;
    } else if (!freeS) freeS = &c;
  }
  bool grace = now - startMs < FP_GRACE_MS;
  if (idx == 0) {
    if (s && !s->bad && s->got < s->total) fpError(sa, pgn);   // el anterior quedó a medias
    if (!s) {
      s = freeS;
      if (!s && oldest) { if (!oldest->bad) fpError(oldest->sa, oldest->pgn); s = oldest; }
      if (!s) return;
    }
    s->used = true; s->bad = false; s->sa = sa; s->pgn = pgn; s->seq = seq; s->next = 1; s->t = now;
    s->total = f.data[1]; s->got = 0;
    if (s->total == 0 || s->total > FP_MAX) {   // longitud imposible
      if (!grace) fpError(sa, pgn);
      s->bad = true; return;
    }
    for (int i = 2; i < f.len && s->got < s->total; i++) s->buf[s->got++] = f.data[i];
  } else {
    if (s && s->bad) {
      if (s->seq == seq) { s->t = now; return; }   // más trozos del mensaje ya descartado
      // trozo de otro mensaje cuyo inicio no hemos visto
    }
    if (!s || s->bad || s->seq != seq || s->next != idx) {
      if (!grace) fpError(sa, pgn);
      if (!s) { s = freeS ? freeS : oldest; if (!s) return; if (s == oldest && !s->bad) fpError(s->sa, s->pgn); }
      s->used = true; s->bad = true; s->sa = sa; s->pgn = pgn; s->seq = seq; s->t = now;
      return;
    }
    s->next++; s->t = now;
    for (int i = 1; i < f.len && s->got < s->total; i++) s->buf[s->got++] = f.data[i];
  }
  if (s->got >= s->total) {
    dev[sa].fpOk++;
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
  // Fast packets huérfanos (sin continuación en 750 ms)
  for (int i = 0; i < FP_SLOTS; i++) {
    FpSlot& c = fp[i];
    if (c.used && now - c.t > FP_TIMEOUT_MS) {
      if (!c.bad) fpError(c.sa, c.pgn);
      c.used = false;
    }
  }
}

bool Core::recentClaimRequest(uint32_t now) const {
  return (claimReqSeen && now - claimReqMs <= CLAIM_WINDOW_MS) ||
         (requestsSentAt != 0 && now - requestsSentAt <= CLAIM_WINDOW_MS);
}

void Core::onMessage(uint8_t sa, uint8_t da, uint32_t pgn, const uint8_t* d, int len, uint32_t now) {
  Device& dv = dev[sa];
  if (pgn == PGN_ADDR_CLAIM && len >= 8) {
    uint64_t name = 0;
    for (int i = 7; i >= 0; i--) name = (name << 8) | d[i];
    bool sameDevice = dv.claims == 0 || name == dv.name;
    if (!sameDevice) { dv.nameChanges++; dv.contentionMs = now; }
    // Reinicio: el MISMO equipo se vuelve a anunciar sin que nadie lo pida, tras
    // estar callado, y sin que otro equipo le acabe de disputar la dirección.
    if (sameDevice && dv.frames > 1 && dv.lastGap >= REBOOT_SILENCE_MS && !recentClaimRequest(now) &&
        !(dv.contentionMs && now - dv.contentionMs <= CLAIM_WINDOW_MS))
      dv.claimsUnrequested++;
    dv.claims++;
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
    if (req == PGN_ADDR_CLAIM) {
      noteClaimRequest(now);   // otro equipo (p. ej. una pantalla) pide las direcciones
      if (da == 255 || da == ownAddr) answerClaimRequest = true;
    }
  } else if (pgn == PGN_PRODUCT_INFO) {
    Info* in = infoFor(sa);
    int n = len > PI_LEN ? PI_LEN : len;
    if (in) { memcpy(in->pi, d, n); in->piLen = n; }
  } else if (pgn == PGN_CONFIG_INFO) {
    Info* in = infoFor(sa);
    int n = len > CI_MAX ? CI_MAX : len;
    if (in) { memcpy(in->ci, d, n); in->ciLen = n; }
  } else if (pgn == PGN_HEARTBEAT && len >= 3) {
    uint8_t seq = d[2];   // 0..252, 255 = no disponible
    if (dv.hbCount > 0) {
      dv.hbSumInt += now - dv.hbLast;
      // la secuencia vuelve atrás sin venir del final (252 → 0) → el equipo se ha reiniciado
      if (seq <= 252 && dv.hbLastSeq <= 252 && seq < dv.hbLastSeq && dv.hbLastSeq < 245) dv.hbResets++;
    }
    dv.hbCount++; dv.hbLastSeq = seq; dv.hbLast = now;
  } else if (pgn == PGN_ENGINE_RAPID && len >= 3) {
    engRpm[d[0] & 7] = (uint16_t)(d[1] | (d[2] << 8));
    engRpmMs[d[0] & 7] = now;
  }

  uint16_t inst = instanceOf(pgn, d, len);
  PgnEntry* e = entry(sa, pgn, inst);
  if (!e) return;
  if (e->n > 0) {
    uint32_t iv = now - e->last;
    if (iv < e->minI) e->minI = iv;
    if (iv > e->maxI) e->maxI = iv;
    e->sumI += iv;
  } else e->first = now;
  e->n++; e->last = now;
  if (e->data >= 0) {
    int n = len > DATA_MAX ? DATA_MAX : len;
    memcpy(pdata[e->data], d, n); plen[e->data] = n;
  }
  if (pgn == PGN_ENGINE_DYN && len >= 24) {
    uint32_t st1 = d[20] | (d[21] << 8), st2 = d[22] | (d[23] << 8);
    // 0xFFFF = campo no disponible: no son alarmas
    uint32_t bits = (st1 == 0xFFFF ? 0 : st1) | ((st2 == 0xFFFF ? 0 : st2) << 16);
    e->orBits |= bits;
    // ¿el motor está girando? (127488 de la misma instancia, > 300 rpm, reciente)
    uint8_t ei = d[0] & 7;
    if (engRpm[ei] != 0xFFFF && engRpm[ei] >= 4 * 300 && now - engRpmMs[ei] < 3000) e->orRun |= bits;
  }
  if (notAvailable(pgn, d, len)) e->naCount++;
}

// ---------------- JSON ----------------
struct Out {
  WriteFn w; void* ctx; char buf[1400]; size_t n = 0;   // ≈ un paquete TCP por trozo
  void flush() { if (n) { w(ctx, buf, n); n = 0; } }
  void put(const char* s) { while (*s) { if (n >= sizeof(buf)) flush(); buf[n++] = *s++; } }
  void f(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void hex(const uint8_t* d, int len) {
    static const char H[] = "0123456789abcdef";
    put("\"");
    for (int i = 0; i < len; i++) { if (n + 2 >= sizeof(buf)) flush(); buf[n++] = H[d[i] >> 4]; buf[n++] = H[d[i] & 15]; }
    put("\"");
  }
  void name(uint64_t v) { f("\"%08x%08x\"", (unsigned)(v >> 32), (unsigned)(v & 0xFFFFFFFF)); }
};
void Out::f(const char* fmt, ...) {
  char t[320]; va_list a; va_start(a, fmt); vsnprintf(t, sizeof(t), fmt, a); va_end(a); put(t);
}

struct Guard {
  LockFn f; void* c;
  void on() { if (f) f(c, true); }
  void off() { if (f) f(c, false); }
};

void Core::writeJson(WriteFn w, void* ctx, uint32_t now, const char* extraLive, LockFn lock, void* lctx, bool lite) {
  Out o; o.w = w; o.ctx = ctx;
  Guard g{lock, lctx};

  // Cabecera (copia rápida con el mutex tomado)
  g.on();
  BusStats b = bus; uint32_t st = startMs, fpe = fpErrTotal, pov = pgOverflow, iov = infoOverflow;
  uint8_t own = ownAddr;
  g.off();
  uint32_t dur = now - st;
  float loadAvg = dur > 0 ? (float)b.bits * 100.0f * 1000.0f / (250000.0f * dur) : 0;
  o.f("{\"t\":%u,\"dur\":%u,\"bus\":{\"frames\":%u,\"loadNow\":%.1f,\"loadAvg\":%.1f,\"loadPeak\":%.1f,\"fpErr\":%u,\"pgOverflow\":%u,\"infoOverflow\":%u,\"ownAddr\":%u}",
      (unsigned)now, (unsigned)dur, (unsigned)b.frames, b.loadNow, loadAvg, b.loadPeak,
      (unsigned)fpe, (unsigned)pov, (unsigned)iov, own);
  if (extraLive && *extraLive) { o.put(","); o.put(extraLive); }

  o.put(",\"devices\":[");
  bool first = true;
  static Device d;      // estáticos: no ocupan pila (writeJson no es reentrante)
  static Info in;
  for (int sa = 0; sa < 256; sa++) {
    g.on();
    bool seen = dev[sa].seen;
    bool hasInfo = false;
    if (seen) {
      d = dev[sa];
      if (d.info >= 0) {
        hasInfo = true;
        in.piLen = info[d.info].piLen; memcpy(in.pi, info[d.info].pi, in.piLen);
        in.ciLen = lite ? 0 : info[d.info].ciLen;
        if (in.ciLen) memcpy(in.ci, info[d.info].ci, in.ciLen);
      }
    }
    g.off();
    if (!seen) continue;
    if (!first) o.put(",");
    first = false;
    o.f("{\"sa\":%d,\"name\":", sa); o.name(d.name);
    o.put(",\"names\":[");
    for (int i = 0; i < d.nNames; i++) { if (i) o.put(","); o.name(d.names[i]); }
    o.f("],\"nameChg\":%u,\"claims\":%u,\"claimsUnreq\":%u,\"first\":%u,\"last\":%u,\"frames\":%u,\"maxGap\":%u,\"gaps5s\":%u,\"fpErr\":%u,\"fpOk\":%u,",
        d.nameChanges, d.claims, d.claimsUnrequested, (unsigned)(d.firstSeen - st), (unsigned)(d.lastSeen - st),
        (unsigned)d.frames, (unsigned)d.maxGap, d.gaps5s, d.fpErrors, (unsigned)d.fpOk);
    o.f("\"hb\":{\"n\":%u,\"resets\":%u,\"avg\":%u},\"pi\":", d.hbCount, d.hbResets,
        (unsigned)(d.hbCount > 1 ? d.hbSumInt / (d.hbCount - 1) : 0));
    o.hex(in.pi, hasInfo ? in.piLen : 0);
    o.put(",\"ci\":");
    o.hex(in.ci, hasInfo ? in.ciLen : 0);
    o.put("}");
  }
  o.put("]");

  if (!lite) {
    o.put(",\"pgns\":[");
    static PgnEntry batch[8];
    static uint8_t bdata[8][DATA_MAX]; static uint8_t blen[8];
    int i = 0;
    for (;;) {
      g.on();
      int n = 0;
      while (n < 8 && i + n < pgUsed) {
        batch[n] = pg[i + n];
        blen[n] = batch[n].data >= 0 ? plen[batch[n].data] : 0;
        if (blen[n]) memcpy(bdata[n], pdata[batch[n].data], blen[n]);
        n++;
      }
      g.off();
      if (!n) break;
      for (int k = 0; k < n; k++) {
        PgnEntry& e = batch[k];
        if (i + k) o.put(",");
        o.f("{\"sa\":%u,\"pgn\":%u,\"inst\":%u,\"n\":%u,\"first\":%u,\"last\":%u,\"minI\":%u,\"maxI\":%u,\"avgI\":%u,\"or\":%u,\"orRun\":%u,\"na\":%u,\"d\":",
            e.sa, (unsigned)e.pgn, e.inst, (unsigned)e.n, (unsigned)(e.first - st), (unsigned)(e.last - st),
            (unsigned)(e.n > 1 ? e.minI : 0), (unsigned)e.maxI, (unsigned)(e.n > 1 ? e.sumI / (e.n - 1) : 0),
            (unsigned)e.orBits, (unsigned)e.orRun, (unsigned)e.naCount);
        o.hex(bdata[k], blen[k]);
        o.put("}");
      }
      i += n;
    }
    o.put("]");
  }
  o.put("}");
  o.flush();
}

}  // namespace n2k
