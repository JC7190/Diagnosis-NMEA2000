// Núcleo de análisis NMEA 2000 — sin dependencias de Arduino (se puede probar en PC)
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace n2k {

// Salida en trozos (en el ESP32 va directa al navegador, en el PC a un string)
typedef void (*WriteFn)(void* ctx, const char* s, size_t n);

struct Frame {
  uint32_t id;      // identificador CAN de 29 bits
  uint8_t len;
  uint8_t data[8];
};

static const int MAX_PGN_ENTRIES = 400;
static const int INFO_SLOTS = 80;
static const int FP_SLOTS = 48;
static const int PI_LEN = 134;
static const int CI_MAX = 180;
static const int DATA_MAX = 48;
static const int NAMES_MAX = 4;

struct Device {
  bool seen;
  uint64_t name;          // último NAME reclamado en esta dirección
  uint64_t names[NAMES_MAX];
  uint8_t nNames;
  uint16_t claims;        // nº de address claims recibidos
  uint16_t claimsUnrequested;
  uint32_t firstSeen, lastSeen;
  uint32_t frames;
  uint32_t maxGap;        // mayor silencio entre tramas (ms)
  uint16_t gaps5s;        // silencios > 5 s
  uint16_t fpErrors;
  int16_t info;           // índice en el pool de Info (-1 = ninguno)
  uint16_t hbCount, hbResets;
  uint8_t hbLastSeq;
  uint32_t hbLast, hbSumInt;
  uint32_t lastClaimMs;
};

struct Info {
  uint8_t pi[PI_LEN]; uint8_t piLen;   // Product Information 126996
  uint8_t ci[CI_MAX]; uint8_t ciLen;   // Configuration Information 126998
};

struct PgnEntry {
  bool used;
  uint8_t sa;
  uint8_t inst;           // instancia (0xFF si no aplica)
  uint32_t pgn;
  uint32_t n;
  uint32_t first, last;
  uint32_t minI, maxI;
  uint64_t sumI;
  uint8_t len;
  uint8_t data[DATA_MAX];
  uint32_t orBits;        // 127489: OR acumulado de los bits de alarma
  uint32_t naCount;       // nº de mensajes con el dato principal "no disponible"
};

struct FpSlot {
  bool used;
  uint8_t sa; uint32_t pgn;
  uint8_t seq; uint8_t next; uint8_t total; uint8_t got;
  uint8_t buf[223];
  uint32_t t;
};

struct BusStats {
  uint32_t frames;
  uint64_t bits;          // bits estimados en el cable
  uint32_t winStart; uint64_t winBits;
  float loadNow, loadPeak;
  uint32_t ownTx, ownTxFail;
};

class Core {
 public:
  Core();
  void reset(uint32_t now);
  // Procesa una trama recibida. Devuelve true si es una petición que hay que contestar (address claim)
  void onFrame(const Frame& f, uint32_t now);
  void tick(uint32_t now);
  // Dirección propia (modo activo)
  uint8_t ownAddr = 251;
  uint64_t ownName = 0;
  bool claimLost = false;   // se pidió cambiar de dirección
  bool answerClaimRequest = false;
  // Escribe el JSON completo del estado
  void writeJson(WriteFn w, void* ctx, uint32_t now, const char* extraLive);

  Device dev[256];
  PgnEntry* pg;            // bloques separados en el heap (evita necesitar un bloque enorme)
  Info* info;
  int infoUsed = 0;
  int pgUsed = 0;
  uint32_t pgOverflow = 0;
  BusStats bus;
  uint32_t startMs = 0;
  uint32_t fpErrTotal = 0;
  uint32_t requestsSentAt = 0;   // marca de tiempo de la última petición activa (ms)

  static void parseId(uint32_t id, uint32_t& pgn, uint8_t& sa, uint8_t& da, uint8_t& prio);
  static uint32_t makeId(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa);
  static bool isFast(uint32_t pgn);
  static bool isProprietary(uint32_t pgn);

 private:
  FpSlot* fp;
  void onMessage(uint8_t sa, uint8_t da, uint32_t pgn, const uint8_t* d, int len, uint32_t now);
  PgnEntry* entry(uint8_t sa, uint32_t pgn, uint8_t inst);
  void touchDevice(uint8_t sa, uint32_t now);
  Info* infoFor(uint8_t sa);
};

uint8_t instanceOf(uint32_t pgn, const uint8_t* d, int len);

}  // namespace n2k
