# N2K Diagnóstico — contexto para Claude Code

Herramienta propia de Javi (técnico/encargado en una empresa de electrónica náutica: Lowrance, Simrad, MotorGuide, WaterSnake) para **escanear y diagnosticar redes NMEA 2000** (pantallas Lowrance, motores eléctricos Ghost/Recon, sondas, GPS, etc.) y generar un **informe PDF** que diga claramente qué falla y qué hay que cambiar.

Se maneja desde un **iPhone (Safari)** conectado a la Wi-Fi que crea el propio ESP32. El aparato se conecta a la red del barco con una T NMEA 2000 y se alimenta de ella (12 V).

Idioma: toda la interfaz, el PDF y los mensajes van en **español** (de España). Javi prefiere respuestas cortas y directas, que no se añadan funciones que no ha pedido, y que lo visual quede cuidado.

---

## Estado actual

- Firmware **v1.1** completo y compilando (≈1,19 MB, 60 % de la partición `min_spiffs`). **No se ha probado aún en hardware real**: el material acaba de llegar y se está montando.
- Probado en PC: simulador de red con averías → JSON → `analyze.js` → PDF. UI probada con Playwright contra un servidor mock.
- Actualización por Wi-Fi (OTA) con rollback automático ya implementada.

## Qué se te pide (Claude Code)

1. **Revisión a fondo del código** (firmware + web): bugs, condiciones de carrera, fugas de memoria, desbordes de buffers, errores de parseo NMEA 2000/fast-packet, falsos positivos/negativos del diagnóstico, robustez ante redes reales grandes y ruidosas.
2. Corregir lo que encuentres y dejar el firmware, la web y el PDF listos para la primera prueba real.
3. Mantener: español, iPhone/Safari como cliente principal, todo servido desde el ESP32 sin internet, OTA funcionando, pines y hardware tal cual (ver abajo). **No añadir funciones nuevas sin preguntar a Javi.** Si ves algo que merece la pena, propónlo como opciones a elegir (Javi quiere las preguntas siempre en formato tipo test).
4. Al terminar: compilar, generar `Diagnostico_N2K_firmware.bin` (merged, para grabar en 0x0) y `Diagnostico_N2K_actualizacion_vX.Y.bin` (app sola, para OTA), y subir `FW_VERSION`.

---

## Hardware (ya comprado, no cambiar)

| Pieza | Modelo |
|---|---|
| MCU | ELEGOO ESP32 DevKit V1, 30 pines, USB-C (ESP32-WROOM-32) |
| Transceptor CAN | SN65HVD230 (placa azul, chip "VP230", bornes de tornillo CANH/CANL, pines 3.3V/GND/CAN RX/CAN TX) |
| Alimentación | Convertidor MEIRIYFA 8–23 V → USB-C 5 V, enchufado al USB-C del ESP32 |
| Divisor de tensión | 100 kΩ (a +12 V) / 20 kΩ (a GND) + 100 nF a GND, al GPIO34 |
| Uniones | Cables dupont hembra-hembra + conectores Wago 221 (sin soldar) |
| Caja | Therlan IP65 de plástico 100×68×50 + prensaestopas PG7/PG9. Sin resina (afecta al Wi-Fi). Tornillos antimanipulación |

**Importante:** la placa SN65HVD230 trae una resistencia de terminación de 120 Ω soldada (SMD marcada "121"). Medía 118 Ω entre CANH-CANL y **hay que quitarla**: el aparato se conecta en una T de una red que ya tiene sus dos terminadores. Los dos pines CANL/CANH junto al borne son solo otra salida, no un jumper.

### Pines

| ESP32 | Conexión |
|---|---|
| GPIO5 (D5) | CAN TX del SN65 |
| GPIO4 (D4) | CAN RX del SN65 |
| GPIO34 (D34) | Punto medio del divisor 100k/20k + 100 nF → `VDIV = 6.0` |
| 3V3 / GND | Alimentación del SN65 (nunca a 5 V) |
| USB-C | Salida del convertidor (nunca a la vez que el cable del ordenador) |
| GPIO2 | LED de la placa |

Cable NMEA 2000 (Micro-C): rojo +12 V (con fusible 1 A), negro GND, blanco CAN-H, azul CAN-L, malla sin conectar.

---

## Arquitectura

```
firmware/n2kdiag/
  n2kdiag.ino      TWAI 250 kbps, tarea CAN en core 0 con mutex, WebServer, softAP, mDNS, OTA
  n2k_core.h/.cpp  Núcleo de análisis (compila también en PC): dispositivos, PGNs, fast-packet,
                   address claim / NAME, product info, config info, heartbeat, estadísticas del bus
  fast_pgns.h      Generado (lista de PGNs fast-packet desde canboat)
  web_assets.h     Generado (web en gzip PROGMEM) — NO editar a mano
web/
  index.html, style.css, app.js   UI (pantallas: inicio, ajustes, en curso, pregunta terminador, resultado)
  analyze.js      Motor de diagnóstico: snapshot JSON → veredicto, hallazgos, acciones, tablas
  report.js       PDF A4 con jsPDF + autotable
  lookups.js      Generado (fabricantes, clases de dispositivo…) — usa `var LK`, no `const`
  update.html     Página de actualización OTA
  lib/            jsPDF y autotable (MIT), servidos desde el ESP32, cargados bajo demanda
tools/
  build_web.py    Inlinea index+css+js en dist/index.html y genera web_assets.h (gzip)
  gen_data.py     canboat.json → fast_pgns.h + lookups.js  (uso: gen_data.py canboat.json .)
test/
  sim.cpp         Red simulada con averías → snapshot JSON
  make_pdf.js     Genera PDF de ejemplo desde un snapshot
  mock_server.py  Sirve la web + snapshot en :8765
  ui_test.py      Playwright (Chrome) recorre la UI
```

### Red y endpoints
- Wi-Fi AP `N2K-Diagnostico` / clave `nmea2000`, IP `192.168.4.1`, mDNS `n2k.local`.
- `GET /` UI · `GET /jspdf.js`, `/autotable.js` · `GET /api/snapshot` (JSON completo + bloque `live`)
- `GET /api/start?dur=<s>&mode=active|passive` · `GET /api/resume`
- `GET /update` · `POST /api/update` (multipart, campo `firmware`)

### Flujo del diagnóstico
- **Modo activo** (por defecto): el ESP32 reclama dirección (60928) y a los 0,4 s pide 60928 a todos, a 0,9 s 126996, a 1,4 s 126998; a los 6 s y 12 s pide 126996 dirigido a quien no respondió; a 20 s repite 60928. **Modo pasivo**: solo escucha (no transmite nunca).
- Al acabar, `twEnd` congela los contadores TWAI (errores, bus-off…) para que el informe sea coherente.
- La app pregunta al usuario (tipo test) cuánto mide el terminador con el multímetro (≈60 Ω correcto, ≈120 Ω falta uno, ≈40 Ω sobra uno…), porque el ESP32 no puede medirlo.
- `analyze.js` genera hallazgos: alimentación (umbrales 9 / 10,5 / 11,5 / 15,5 V, rizado > 1,5 V), terminación, tasa de errores CAN (> 0,3 % aviso, > 3 % fallo, bus-off), carga del bus, dirección 254, conflictos de dirección, caídas/reinicios (heartbeat), errores fast-packet, falta de product info, instancias duplicadas, múltiples fuentes de un dato, frecuencias de PGN anómalas, bits de alarma de motor (127489), batería (127508), datos "no disponible" de profundidad/GPS/rumbo, LEN.

### OTA
- Partición `min_spiffs` (app 1.966.080 bytes, dos slots OTA).
- El core 3.3.x tiene `CONFIG_APP_ROLLBACK_ENABLE=y`. Se sobrescribe `bool verifyRollbackLater(){return true;}` y en `loop()` se llama a `esp_ota_mark_app_valid_cancel_rollback()` tras 30 s estables. Si el firmware nuevo se cuelga antes, vuelve al anterior.
- Durante la subida se pausa el análisis.

---

## Compilar

Arduino core **esp32 3.3.x**, placa *ESP32 Dev Module*, partición **min_spiffs**:

```bash
python3 tools/build_web.py            # siempre que cambie algo en web/
arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=min_spiffs \
  --output-dir build firmware/n2kdiag
# build/n2kdiag.ino.merged.bin → grabar en 0x0 (primera vez, por USB)
# build/n2kdiag.ino.bin        → actualización OTA desde /update
```

Notas del entorno donde se creó (puede que no te afecte):
- Se compiló con `--build-property runtime.tools.ctags.path=<ctags falso>` porque no había ctags; por eso **los prototipos de funciones del .ino están declarados a mano**. Si los mueves, mantenlos.
- `Core` se crea en el heap (`new n2k::Core()`) y sus arrays grandes van en bloques separados: en estático desbordaba `.dram0.bss`.

Grabación inicial: https://espressif.github.io/esptool-js en Chrome, dirección 0x0, archivo merged.

## Pruebas en PC
```bash
cd test
g++ -std=c++17 -I../firmware/n2kdiag sim.cpp ../firmware/n2kdiag/n2k_core.cpp -o sim && ./sim > snapshot.json
npm i jspdf jspdf-autotable && node make_pdf.js ./snapshot.json 120 informe.pdf
python3 mock_server.py 8765   # y python3 ui_test.py
```

---

## Puntos a revisar con especial cuidado
- Reensamblado fast-packet con varios emisores intercalados y secuencias perdidas (`FP_SLOTS=48`).
- Límites: `MAX_PGN_ENTRIES=400`, `INFO_SLOTS=80`, `PI_LEN=134`, `CI_MAX=180`, `DATA_MAX=48`; qué pasa al llenarse.
- `writeJson` con muchas entradas: tamaño, memoria y tiempo con el mutex tomado (no bloquear la tarea CAN).
- Conteo de "address claim no solicitado" (solo si `dv.frames > 1`) y detección de reinicios por heartbeat: evitar falsos positivos.
- Umbral de "dispositivo callado" basado en su intervalo típico.
- Que el modo pasivo no transmita nada en ningún caso (ni address claim).
- Lectura de tensión en GPIO34 (ADC no lineal del ESP32; ¿calibración con `analogReadMilliVolts`?).
- Seguridad de `/api/update`: aceptable para uso en taller con AP con clave, pero comprobar que un .bin no válido se rechaza sin brickear.
- Safari iOS: descarga del PDF, captive portal, reconexión tras reinicio OTA.
