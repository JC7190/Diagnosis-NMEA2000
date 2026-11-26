// Núcleo de análisis NMEA 2000 — sin dependencias de Arduino (se puede probar en PC)
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace n2k {

// Salida en trozos (en el ESP32 va directa al navegador, en el PC a un string)
typedef void (*WriteFn)(void* ctx, const char* s, size_t n);
// Bloqueo opcional: writeJson solo toma el mutex para copiar cada registro,
// nunca mientras escribe en la red (así no frena la tarea CAN)
typedef void (*LockFn)(void* ctx, bool lock);

struct Frame {
  uint32_t id;      // identificador CAN de 29 bits
  uint8_t len;
  uint8_t data[8];
};

static const int MAX_PGN_ENTRIES = 400;
static const int PG_HASH = 512;        // potencia de 2
static const int INFO_SLOTS = 80;
static const int FP_SLOTS = 96;         // mensajes largos en curso a la vez (redes grandes)
static const int FP_MAX = 223;         // tamaño máximo de un fast packet
static const int PI_LEN = 134;
static const int CI_MAX = 223;         // 126998: tres textos de hasta 70 caracteres
static const int DATA_MAX = 48;
static const int DATA_SLOTS = 48;      // contenido guardado solo de unos pocos PGN
static const int NAMES_MAX = 4;
static const uint32_t FP_TIMEOUT_MS = 750;
static const uint32_t FP_GRACE_MS = 1000;      // al empezar se ven trozos de mensajes ya empezados
static const uint32_t CLAIM_WINDOW_MS = 2000;  // respuestas a una petición de address claim
static const uint32_t REBOOT_SILENCE_MS = 400; // un reinicio deja al equipo callado al menos esto

struct Device {
  bool seen;
  uint64_t name;          // último NAME reclamado en esta dirección
  uint64_t names[NAMES_MAX];
  uint8_t nNames;
  uint16_t nameChanges;   // veces que cambió el NAME en esta dirección (conflicto si se repite)
  uint16_t claims;        // nº de address claims recibidos
  uint16_t claimsUnrequested;
  uint32_t firstSeen, lastSeen;
  uint32_t frames;
  uint32_t maxGap;        // mayor silencio entre tramas (ms)
  uint32_t lastGap;       // silencio justo antes de la trama actual
  uint16_t gaps5s;        // silencios > 5 s
  uint16_t fpErrors;      // mensajes fast packet incompletos (uno por mensaje)
  uint32_t fpOk;          // mensajes fast packet completos
  int16_t info;           // índice en el pool de Info (-1 = ninguno)
  uint16_t hbCount, hbResets;
  uint8_t hbLastSeq;
  uint32_t hbLast, hbSumInt;
  uint32_t lastClaimMs;
  uint32_t contentionMs;  // último address claim de otro NAME en esta dirección
};

struct Info {
  uint8_t pi[PI_LEN]; uint8_t piLen;   // Product Information 126996
  uint8_t ci[CI_MAX]; uint8_t ciLen;   // Configuration Information 126998
};

struct PgnEntry {
  uint8_t sa;
  uint16_t inst;          // instancia (0xFF si no aplica; +fuente<<8 en temperaturas/presión/humedad)
  int16_t next;           // siguiente en la cadena del hash
  uint32_t pgn;
  uint32_t n;
  uint32_t first, last;
  uint32_t minI, maxI;
  uint64_t sumI;
  uint32_t orBits;        // 127489: OR acumulado de los bits de alarma
  uint32_t orRun;         // 127489: lo mismo, solo con el motor girando
  uint32_t naCount;       // nº de mensajes con el dato principal "no disponible"
  int8_t data;            // índice en el pool de contenido (-1 = no se guarda)
};

struct FpSlot {
  bool used;
  bool bad;               // mensaje roto: se descartan sus trozos sin volver a contarlo
  uint8_t sa; uint32_t pgn;
  uint8_t seq; uint8_t next; uint8_t total; uint8_t got;
  uint8_t buf[FP_MAX];
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
  void onFrame(const Frame& f, uint32_t now);
  void tick(uint32_t now);
  // Dirección propia (modo activo)
  uint8_t ownAddr = 251;
  uint64_t ownName = 0;
  bool claimLost = false;   // otro equipo con NAME prioritario reclamó nuestra dirección
  bool answerClaimRequest = false;
  // Avisa al núcleo de que se ha enviado/visto una petición de address claim
  void noteClaimRequest(uint32_t now) { claimReqMs = now; claimReqSeen = true; }
  // Avisa de que nosotros reclamamos ownAddr (el dueño anterior puede defenderla)
  void noteOwnClaim(uint32_t now) { dev[ownAddr].contentionMs = now; }
  // Escribe el JSON. lite = sin tabla de PGNs ni textos de configuración (para la vista en directo)
  void writeJson(WriteFn w, void* ctx, uint32_t now, const char* extraLive,
                 LockFn lock = nullptr, void* lctx = nullptr, bool lite = false);

  Device dev[256];
  PgnEntry* pg;            // bloques separados en el heap (evita necesitar un bloque enorme)
  uint8_t (*pdata)[DATA_MAX];
  uint8_t* plen;
  int dataUsed = 0;
  Info* info;
  int infoUsed = 0;
  uint32_t infoOverflow = 0;
  int pgUsed = 0;
  uint32_t pgOverflow = 0;
  BusStats bus;
  uint32_t startMs = 0;
  uint32_t fpErrTotal = 0;
  uint32_t requestsSentAt = 0;   // compatibilidad: última petición propia (ms)

  static void parseId(uint32_t id, uint32_t& pgn, uint8_t& sa, uint8_t& da, uint8_t& prio);
  static uint32_t makeId(uint8_t prio, uint32_t pgn, uint8_t da, uint8_t sa);
  static bool isFast(uint32_t pgn);
  static bool isProprietary(uint32_t pgn);

 private:
  FpSlot* fp;
  int16_t* pgHead;
  uint32_t claimReqMs = 0; bool claimReqSeen = false;
  uint16_t engRpm[8]; uint32_t engRpmMs[8];   // última velocidad de cada motor (127488)
  void onMessage(uint8_t sa, uint8_t da, uint32_t pgn, const uint8_t* d, int len, uint32_t now);
  PgnEntry* entry(uint8_t sa, uint32_t pgn, uint16_t inst);
  void touchDevice(uint8_t sa, uint32_t now);
  Info* infoFor(uint8_t sa);
  void fpError(uint8_t sa, uint32_t pgn);
  bool recentClaimRequest(uint32_t now) const;
};

uint16_t instanceOf(uint32_t pgn, const uint8_t* d, int len);
bool keepsData(uint32_t pgn);

}  // namespace n2k
