# Créditos y licencias de terceros

PoweryMax Nmea2000 Diagnostic usa estas piezas de terceros:

| Pieza | Uso | Licencia |
|---|---|---|
| **CANboat** (© 2009-2026 Kees Verruijt) | Tablas de fabricantes, funciones de equipo, nombres e intervalos de PGN y lista de PGN fast-packet (`web/lookups.js`, `firmware/n2kdiag/fast_pgns.h`, generados con `tools/gen_data.py`) | Apache-2.0 — ver `canboat_LICENSE.txt` y `Apache-2.0.txt` |
| **jsPDF** y **jsPDF-AutoTable** | Generación del informe PDF en el iPhone (`web/lib/`) | MIT (aviso incluido en cada archivo) |
| **ESP-IDF / Arduino-ESP32** (Espressif) | Plataforma del firmware | Apache-2.0 / LGPL-2.1 |

Las tablas de CANboat se han obtenido por ingeniería inversa de equipos reales; no son la norma oficial NMEA 2000.
"NMEA 2000" es marca registrada de la National Marine Electronics Association; esta herramienta no está certificada por NMEA.
