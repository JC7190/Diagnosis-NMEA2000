// Umbrales del diagnóstico — fichero ÚNICO para ajustarlos sin tocar la lógica.
// Cada umbral lleva su fuente y su etiqueta: verificado | estimado | supuesto.
// Referencia: especificación técnica NMEA 2000 del proyecto (§2.5, §2.6, §9.2).
var CFG = {
  tension: {   // PHY-01 Tensión de red (NET-S − NET-C)
    falloBajo: 9.0, avisoBajo: 10.0, falloAlto: 15.75, seguridad24V: 16.0,
    fuente: 'PEAK-MiniDiag NMEA 2000 Ap. E / Maretron N2KMeter; 9–16 V Garmin', etiqueta: 'verificado',
  },
  rizado: {    // PHY-03 Rizado/transitorios pico a pico, medido a ráfagas (≈300 lecturas/s,
    // pico a pico de cada ventana de 50 ms sobre medias de 2 lecturas)
    avisoPP: 2.0, falloPP: 5.0,
    fuente: 'PEAK-MiniDiag Ap. E (umbral); medida propia a ráfagas', etiqueta: 'estimado',
  },
  errores: {   // LNK-01 / LNK-02 Errores de bus
    avisoTotal: 1, falloPorSegundo: 15,
    fuente: 'PEAK-MiniDiag Ap. E; Maretron N2KMeter §7', etiqueta: 'verificado',
  },
  carga: {     // LNK-04 Carga de bus (media de la prueba, régimen estable)
    aviso: 80, fallo: 90,
    bitsPorError: 80,   // trama cortada (≈ media trama) + trama de error; estimado
    fuente: 'PEAK-MiniDiag Ap. E; Maretron N2KMeter §8', etiqueta: 'verificado',
  },
  fastPacket: { // TRN-01 Mensajes largos incompletos por equipo
    avisoPct: 1, falloPct: 5, minMensajes: 2,
    fuente: 'Valor de diseño; ajustar con capturas reales', etiqueta: 'supuesto',
  },
  consumo: {   // PHY-10 Consumo declarado (1 LEN = 50 mA); red Micro máx. 3 A
    avisoLEN: 60,
    fuente: 'Soundings, NMEA 2000 primer; Garmin', etiqueta: 'verificado',
  },
  nodos: {     // Topología: nodos físicos máximos
    max: 50,
    fuente: 'Actisense, 3 fundamentals', etiqueta: 'verificado',
  },
  terminacion: { // PHY-08 / §2.6 Medida manual con la red apagada (prevalece sobre estimaciones)
    fuente: 'Actisense Advanced diagnostics; PEAK-MiniDiag §Termination', etiqueta: 'verificado',
  },
};
if (typeof module !== 'undefined') module.exports = CFG;
