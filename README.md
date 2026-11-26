# Diagnóstico NMEA 2000 (ESP32)

Herramienta de taller que se conecta a una red NMEA 2000, la analiza y genera un informe PDF con lo que falla y qué hay que cambiar. Se maneja desde el iPhone (Safari) o desde cualquier ordenador, a través de la Wi-Fi que crea el propio aparato.

- Wi-Fi: `N2K-Diagnostico` · clave `nmea2000`
- Web: http://192.168.4.1 (o http://n2k.local)

## Estructura
| Carpeta | Contenido |
|---|---|
| `firmware/n2kdiag/` | Sketch del ESP32 (Arduino). `n2k_core.*` es el análisis de tramas y se puede compilar también en PC |
| `web/` | Interfaz (`index.html`, `style.css`, `app.js`), motor de diagnóstico (`analyze.js`) y PDF (`report.js`) |
| `tools/` | `gen_data.py` (tablas desde canboat) y `build_web.py` (empaqueta la web en `web_assets.h`) |
| `test/` | Simulador de red con averías, servidor de prueba y generador de PDF de ejemplo |

## Compilar
1. `python3 tools/build_web.py` (cada vez que cambies algo de `web/`)
2. Arduino IDE o arduino-cli con el core **esp32 3.3.x**, placa *ESP32 Dev Module*:
   `arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=min_spiffs firmware/n2kdiag` (partición con OTA)

## Pines
| ESP32 | Conexión |
|---|---|
| GPIO5 | CTX del SN65HVD230 |
| GPIO4 | CRX del SN65HVD230 |
| GPIO34 | Divisor de tensión 100k (a +12 V) / 20k (a GND) + 100 nF |
| 3V3 / GND | Alimentación del SN65HVD230 |
| USB-C | Salida del convertidor 12 V → USB-C |

## Pruebas en PC
```
cd test
g++ -std=c++17 -I../firmware/n2kdiag sim.cpp ../firmware/n2kdiag/n2k_core.cpp -o sim && ./sim > snapshot.json
node make_pdf.js ./snapshot.json 120 informe.pdf   # necesita jspdf y jspdf-autotable
python3 mock_server.py 8765                        # web en http://127.0.0.1:8765
```

Tablas de fabricantes y PGNs: proyecto canboat (Apache-2.0). PDF: jsPDF + jspdf-autotable (MIT).
