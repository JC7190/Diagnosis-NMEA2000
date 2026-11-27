# PoweryMax Nmea2000 Diagnostic — contexto para Claude Code

Herramienta propia de Javi (técnico/encargado en una empresa de electrónica náutica: Lowrance, Simrad, MotorGuide, WaterSnake) para **escanear y diagnosticar redes NMEA 2000** (pantallas Lowrance, motores eléctricos Ghost/Recon, sondas, GPS, etc.) y generar un **informe PDF** que diga claramente qué falla y qué hay que cambiar.

Se maneja desde un **iPhone (Safari)** conectado a la Wi-Fi que crea el propio ESP32. El aparato se conecta a la red del barco con una T NMEA 2000 y se alimenta de ella (12 V).

Idioma: toda la interfaz, el PDF y los mensajes van en **español** (de España). Javi prefiere respuestas cortas y directas, que no se añadan funciones que no ha pedido, y que lo visual quede cuidado.

---

## Estado actual

- Firmware **v1.6** compilando con core esp32 3.3.13 (≈1,23 MB, 62 % de `min_spiffs`). Se graba por USB desde el Mac de Javi (esptool en ~/CLAUDE/N2K, puerto /dev/cu.usbserial-0001). **No se ha probado aún en hardware real.**
- Probado en PC: `test/run_tests.js` (3 escenarios del simulador: averías, red sana con trampas de falsos positivos, red grande y ruidosa) y `test/ui_test.js` (Playwright, iPhone 13) contra el mock.
- Binarios listos en `release/`.

### Cambios de la v1.6
- **Captura de tramas en CSV**: búfer circular de 2048 tramas en el ESP32 (≈40 KB, `malloc` al arrancar), endpoint `GET /api/frames?from=N` (texto; 1.ª línea `#seq=<total>;lost=<n>`). La app lo pide cada 0,5 s durante la prueba y en el resultado ofrece «Descargar captura de tramas (CSV)» con formato `marca_tiempo_us;id29_hex;dlc;datos_hex;flags` (huecos marcados como `HUECO`).
- **PDF**: «Resultado por áreas» (agrupado de lo físico a los datos, seguridad primero), sección «Limitaciones de la prueba» y tabla «Fuentes de los umbrales» (de `config.js`).
- Pendiente de decidir con Javi: datos de canboat (seguir + aviso de licencia, o tablas propias) y búsqueda guiada «half-split».

### Cambios de la v1.5
- Pantalla de terminadores: caras reales del conector Micro-C **macho y hembra** (figura 3 de NMEA 2000) con colores por pin (1 malla gris, 2 rojo +12 V, 3 negro GND, 4 blanco CAN-H, 5 azul CAN-L), explicación de los dos terminadores y respuesta nueva «El valor salta» (H/L cruzados o mal contacto).
- **Umbrales centralizados en `web/config.js`**, cada uno con fuente y etiqueta verificado/estimado/supuesto, según la especificación técnica de Javi (§2.5, §9.2): tensión OK 10–15,75 / aviso 9–10 / fallo <9 o >15,75, **>16 V = aviso de seguridad el primero**; rizado p-p (≈2× salto) aviso >2 V, fallo >5 V; errores de bus: cualquier error = aviso, >15/s = fallo; carga media aviso ≥80 %, fallo ≥90 %; fast packet incompletos aviso >1 %, fallo >5 %; consumo aviso >60 LEN (3 A); >50 equipos = aviso.
- Comprobaciones nuevas: estado «NO MEDIDO» (gris), PGN fuera de los rangos NMEA 2000 (LNK-06), NAME duplicado a la vez (NET-03), aviso de banco (un solo equipo en «solo escuchar» → errores de ACK que no son avería).

### Cambios de la v1.4 (marca PoweryMax)
- Producto: **PoweryMax Nmea2000 Diagnostic**. Colores de marca: negro, gris y rojo (`--ac:#e1252d`); los colores de estado (verde/ámbar/rojo) no cambian.
- Logo: `web/brand/PoweryMax_logo.svg` (original). `build_web.py` inserta `web/brand/logo_inline.svg` (coordenadas redondeadas, `currentColor`) donde pone `<!--LOGO-->` en index.html y update.html. `web/brand.js` lleva el logo en PNG para el PDF; se sirve en `/brand.js` y se carga junto con jsPDF.
- Wi-Fi `PoweryMax Nmea2000 Diagnostic`, mDNS `powerymax.local`, nombre en la red NMEA 2000 (126996) `PoweryMax N2K Diag`. Los nombres de archivo (PDF y .bin) no cambian.

### Cambios de la v1.3
- Inicio: aviso de estado en lenguaje sencillo (sin conexión / no llega nada / hay tensión pero no datos / red detectada).
- Prueba en curso: botón "Cancelar prueba" (con confirmación).
- Tras la pregunta del terminador, solo si alguna sonda no da profundidad, se pregunta si el transductor está en el agua (Sí → avería, No → informativo, No lo sé → revisar).
- En prueba completa la herramienta contesta a las peticiones de información de producto (126996, se anuncia como "N2K Diagnostico") y responde NAK (59392) a peticiones dirigidas de otros PGN.
- Memoria libre visible en /update; aviso informativo en el informe si baja de 25 KB.

### Cambios de la v1.2 (resumen)
- El driver TWAI solo se toca desde la tarea CAN (antes la web lo reinstalaba en paralelo → posible cuelgue). La tarea CAN va en el **núcleo 1**, procesa tramas por lotes y siempre hace sus tareas periódicas aunque la red vaya llena.
- Al arrancar y tras "Nueva prueba" el aparato está en **solo escucha**; solo transmite durante una prueba completa. Solo contesta a peticiones de dirección si ya ha reclamado una.
- `/api/snapshot` se envía por trozos (chunked) tomando el mutex solo para copiar cada registro; `?lite=1` omite la tabla de PGN (la app la usa en directo).
- Fast packet: un error por mensaje roto (no uno por trozo), 1 s de gracia al empezar, 96 huecos. En redes de más de 24 equipos las peticiones 126996/126998 van dirigidas y espaciadas 20 ms.
- Diagnóstico sin falsos positivos: reinicios solo si el mismo NAME se reanuncia tras silencio y sin petición de nadie; conflictos solo si la dirección cambia de dueño repetidamente; alarmas de motor "normales con motor parado" (aceite, carga…) solo cuentan con rpm > 300; instancias de temperatura con su fuente; baterías de 24/36/48 V; HDOP/LEN "no disponible"; equipos que cambian de dirección o mandan ráfagas (AIS).
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
  n2kdiag.ino      TWAI 250 kbps, tarea CAN en core 1 con mutex (dueña única del driver), WebServer, softAP, mDNS, OTA
  n2k_core.h/.cpp  Núcleo de análisis (compila también en PC): dispositivos, PGNs, fast-packet,
                   address claim / NAME, product info, config info, heartbeat, estadísticas del bus
  fast_pgns.h      Generado (lista de PGNs fast-packet desde canboat)
  web_assets.h     Generado (web en gzip PROGMEM) — NO editar a mano
web/
  index.html, style.css, app.js   UI (pantallas: inicio, ajustes, en curso, pregunta terminador, pregunta sonda, resultado)
  analyze.js      Motor de diagnóstico: snapshot JSON → veredicto, hallazgos, acciones, tablas
  report.js       PDF A4 con jsPDF + autotable
  lookups.js      Generado (fabricantes, clases de dispositivo…) — usa `var LK`, no `const`
  update.html     Página de actualización OTA
  lib/            jsPDF y autotable (MIT), servidos desde el ESP32, cargados bajo demanda
tools/
  build_web.py    Inlinea index+css+js en dist/index.html y genera web_assets.h (gzip)
  gen_data.py     canboat.json → fast_pgns.h + lookups.js  (uso: gen_data.py canboat.json .)
test/
  sim.cpp         Red simulada (averias | limpia | grande) → snapshot JSON
  run_tests.js    Pruebas automáticas del núcleo y del diagnóstico
  make_pdf.js     Genera PDF de ejemplo desde un snapshot
  mock_server.py  Sirve la web + snapshot en :8765
  ui_test.js      Playwright (iPhone) recorre la UI
```

### Red y endpoints
- Wi-Fi AP `PoweryMax Nmea2000 Diagnostic` / clave `nmea2000`, IP `192.168.4.1`, mDNS `powerymax.local`.
- `GET /` UI · `GET /jspdf.js`, `/autotable.js` · `GET /api/snapshot` (JSON completo + bloque `live`; `?lite=1` sin PGNs)
- `GET /api/start?dur=<s>&mode=active|passive` · `GET /api/resume` · `GET /api/frames?from=N` (captura)
- `GET /update` · `POST /api/update` (multipart, campo `firmware`)

### Flujo del diagnóstico
- **Modo activo** (por defecto): el ESP32 reclama dirección (60928) y a los 0,4 s pide 60928 a todos, a 0,9 s 126996, a 1,4 s 126998; a los 6 s y 12 s pide 126996 dirigido a quien no respondió; a 20 s repite 60928. **Modo pasivo**: solo escucha (no transmite nunca).
- Al acabar, `twEnd` congela los contadores TWAI (errores, bus-off…) para que el informe sea coherente.
- La app pregunta al usuario (tipo test) cuánto mide el terminador con el multímetro (≈60 Ω correcto, ≈120 Ω falta uno, ≈40 Ω sobra uno…), porque el ESP32 no puede medirlo.
- `analyze.js` genera hallazgos (umbrales en `web/config.js`): alimentación, terminación, errores CAN (cualquier error aviso, > 15/s fallo, bus-off), carga del bus, dirección 254, conflictos de dirección, caídas/reinicios (heartbeat), errores fast-packet, falta de product info, instancias duplicadas, múltiples fuentes de un dato, frecuencias de PGN anómalas, bits de alarma de motor (127489), batería (127508), datos "no disponible" de profundidad/GPS/rumbo, LEN.

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
node run_tests.js              # simulador + diagnóstico (3 escenarios)
python3 mock_server.py 8765   # y node ui_test.js <carpeta> (Playwright)
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
