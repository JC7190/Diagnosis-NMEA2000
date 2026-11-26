// Motor de diagnóstico: convierte los datos crudos del ESP32 en hallazgos claros.
// Funciona igual en el navegador y en Node (para pruebas).
(function (root) {
  const LK = root.LK || (typeof require !== 'undefined' ? require('./lookups.js') : null);

  // ---------- utilidades ----------
  const hexToBytes = (h) => { const b = []; h = h || ''; for (let i = 0; i + 1 < h.length; i += 2) b.push(parseInt(h.substr(i, 2), 16)); return b; };
  const u16 = (b, o) => b[o] | (b[o + 1] << 8);
  const s16 = (b, o) => { const v = u16(b, o); return v & 0x8000 ? v - 0x10000 : v; };
  const fmt = (x, d = 1) => (x === null || x === undefined || isNaN(x)) ? '-' : Number(x).toFixed(d).replace('.', ',');
  const secs = (ms) => fmt(ms / 1000, ms < 10000 ? 1 : 0) + ' s';
  const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;
  const isProp = (p) => p === 61184 || (p >= 65280 && p <= 65535) || p === 126720 || (p >= 130816 && p <= 131071);

  function str(b, o, len) {
    let s = '';
    for (let i = o; i < o + len && i < b.length; i++) {
      const c = b[i];
      if (c === 0xFF || c === 0) break;
      if (c >= 32 && c !== 127) s += String.fromCharCode(c);
    }
    return s.replace(/@+$/, '').trim();
  }

  function decodeName(hex) {
    if (!hex || hex.length !== 16) return null;
    const hi = parseInt(hex.substr(0, 8), 16) >>> 0, lo = parseInt(hex.substr(8, 8), 16) >>> 0;
    if (!hi && !lo) return null;
    return {
      uniq: lo & 0x1FFFFF, mfr: (lo >>> 21) & 0x7FF,
      inst: hi & 0xFF, fn: (hi >>> 8) & 0xFF, cls: (hi >>> 17) & 0x7F,
      sysInst: (hi >>> 24) & 0xF, industry: (hi >>> 28) & 7,
    };
  }

  // Product Information (126996). Algunos equipos la mandan más corta: se
  // descodifica lo que haya en vez de darla por perdida.
  function decodePI(hex) {
    const b = hexToBytes(hex);
    if (b.length < 8) return null;
    const pi = {
      n2kVer: u16(b, 0) === 0xFFFF ? '-' : fmt(u16(b, 0) / 1000, 3), code: u16(b, 2),
      model: str(b, 4, 32), sw: str(b, 36, 32), ver: str(b, 68, 32), serial: str(b, 100, 32),
      cert: b.length > 132 ? b[132] : null, len: b.length > 133 && b[133] !== 0xFF ? b[133] : 0,
    };
    return pi;
  }

  function decodeCI(hex) {
    const b = hexToBytes(hex);
    const out = []; let o = 0;
    for (let k = 0; k < 3 && o + 1 < b.length; k++) {
      const len = b[o], type = b[o + 1];
      if (len < 2 || len === 0xFF) { out.push(''); o += 2; continue; }
      let s = '';
      if (type === 1) s = str(b, o + 2, len - 2);
      else for (let i = o + 2; i + 1 < o + len && i + 1 < b.length; i += 2) { const c = u16(b, i); if (c === 0 || c === 0xFFFF) break; s += String.fromCharCode(c); }
      out.push(s.trim()); o += len;
    }
    return { inst1: out[0] || '', inst2: out[1] || '', mfrInfo: out[2] || '' };
  }

  const CLS_ES = { 10: 'Herramientas', 20: 'Seguridad', 25: 'Pasarela de red', 30: 'Distribución eléctrica', 35: 'Generación eléctrica',
    40: 'Dirección / control', 50: 'Propulsión', 60: 'Navegación', 70: 'Comunicación', 75: 'Interfaz de sensores',
    80: 'Instrumentación', 85: 'Entorno exterior', 90: 'Entorno interior', 100: 'Cubierta / pesca', 110: 'Interfaz de usuario', 120: 'Pantalla', 125: 'Entretenimiento' };
  const FN_ES = { '120_130': 'Pantalla', '120_140': 'Avisador de alarmas', '60_145': 'GPS', '60_130': 'Sonda de profundidad',
    '60_135': 'Sonda prof./velocidad', '60_136': 'Sonda prof./vel./temp.', '60_140': 'Sensor de actitud', '60_155': 'Corredera',
    '60_160': 'Indicador de giro', '60_170': 'Navegación integrada', '60_190': 'Gestión de navegación', '60_195': 'AIS', '60_200': 'Radar',
    '40_130': 'Mando de seguimiento', '40_140': 'Selector de modo', '40_150': 'Piloto automático', '40_155': 'Timón', '40_160': 'Compás / rumbo',
    '40_170': 'Flaps', '50_140': 'Motor', '50_150': 'Centralita de motor', '50_155': 'Motor eléctrico', '50_160': 'Pasarela de motor', '50_165': 'Transmisión',
    '50_170': 'Acelerador / cambio', '50_190': 'Interfaz de relojes', '35_170': 'Batería', '35_145': 'Fuente de alimentación de red',
    '35_160': 'Cargador de baterías', '35_180': 'Pasarela de motor', '35_140': 'Motor', '25_130': 'Puente NMEA 2000', '25_135': 'Pasarela 0183-2000',
    '25_136': 'Pasarela NMEA 2000', '25_140': 'Router', '25_150': 'Puente', '25_160': 'Repetidor', '10_130': 'Diagnóstico', '10_140': 'Registrador de tráfico',
    '85_130': 'Meteorología', '85_160': 'Sensor de viento', '75_130': 'Interfaz de sensores', '110_130': 'Botonera', '110_135': 'Panel de control' };
  const PGN_ES = { 60928: 'Reclamación de dirección', 59904: 'Petición ISO', 59392: 'Confirmación ISO', 126993: 'Latido (heartbeat)', 126996: 'Información de producto',
    126998: 'Información de configuración', 126464: 'Lista de PGNs', 127488: 'Motor (rápido)', 127489: 'Motor (dinámico)', 127493: 'Transmisión',
    127505: 'Nivel de depósito', 127508: 'Batería', 127506: 'Estado de batería', 127250: 'Rumbo', 127251: 'Velocidad de giro', 127257: 'Actitud', 127258: 'Variación magnética',
    127245: 'Timón', 128259: 'Velocidad', 128267: 'Profundidad', 128275: 'Distancia recorrida', 129025: 'Posición (rápida)', 129026: 'COG y SOG',
    129029: 'Datos GNSS', 129033: 'Fecha y hora', 129539: 'Precisión GNSS (DOP)', 129540: 'Satélites a la vista', 130306: 'Viento',
    130310: 'Ambiente', 130312: 'Temperatura', 130313: 'Humedad', 130316: 'Temperatura (ampliada)', 130314: 'Presión', 126992: 'Hora del sistema',
    129283: 'Error de derrota', 129284: 'Datos de navegación', 129285: 'Ruta / waypoints', 127237: 'Control de rumbo/derrota', 126208: 'Comando de grupo' };
  const TEMP_SRC = ['mar', 'exterior', 'interior', 'sala de máquinas', 'cabina', 'vivero', 'cebo', 'nevera', 'calefacción', 'punto de rocío',
    'sensación térmica', 'sensación térmica', 'índice de calor', 'congelador', 'gases de escape', 'sello del eje'];
  const SRC_PGNS = [130312, 130313, 130314, 130315, 130316];

  // Bits de alarma de 127489 (Discrete Status 1 y 2). El tercer valor indica las
  // alarmas que salen SIEMPRE con el contacto dado y el motor parado.
  const ENG_BITS = [
    ['Check engine (aviso general de motor)', 'fail'], ['Sobretemperatura', 'fail'], ['Presión de aceite baja', 'fail', 1], ['Nivel de aceite bajo', 'fail'],
    ['Presión de combustible baja', 'fail', 1], ['Tensión del sistema baja', 'warn', 1], ['Nivel de refrigerante bajo', 'fail'], ['Flujo de agua (refrigeración)', 'fail', 1],
    ['Agua en el combustible', 'fail'], ['Indicador de carga (alternador)', 'warn', 1], ['Precalentamiento', null], ['Presión de turbo alta', 'warn'],
    ['Límite de revoluciones superado', 'warn'], ['Sistema EGR', 'warn'], ['Sensor de posición del acelerador', 'fail'], ['Parada de emergencia', 'fail'],
    ['Aviso nivel 1', 'warn'], ['Aviso nivel 2', 'fail'], ['Reducción de potencia', 'fail'], ['Mantenimiento necesario', 'warn'],
    ['Error de comunicación del motor', 'warn'], ['Acelerador secundario', null], ['Protección de arranque en punto muerto', null], ['Motor apagándose', 'fail', 1],
  ];

  function mfrName(c) { return (LK && LK.mfr[c]) || ('Fabricante ' + c); }
  function fnName(cls, fn) { return FN_ES[cls + '_' + fn] || (LK && LK.fn[cls + '_' + fn]) || CLS_ES[cls] || ('Clase ' + cls); }
  function pgnName(p, mfr) {
    if (PGN_ES[p]) return PGN_ES[p];
    if (isProp(p)) return 'Datos propietarios' + (mfr ? ' (' + mfr + ')' : '');
    return (LK && LK.pgn[p] && LK.pgn[p].d) || 'PGN ' + p;
  }
  // Texto de la instancia de un mensaje
  function instText(pgn, inst) {
    if (inst === 255 || inst === undefined || inst === null) return '';
    if (pgn === 127505) return `${inst & 15} (tipo ${inst >> 4})`;
    if (pgn === 130306) return ['verdadero (rumbo)', 'magnético', 'aparente', 'verdadero (barco)', 'verdadero (agua)'][inst] || String(inst);
    if (SRC_PGNS.includes(pgn)) {
      const i = inst & 0xFF, src = inst >> 8;
      return pgn === 130313 || pgn === 130314 || pgn === 130315 ? `${i}, fuente ${src}` : `${i}, ${TEMP_SRC[src] || 'fuente ' + src}`;
    }
    return String(inst);
  }

  // ---------- análisis ----------
  function analyze(snap, manual) {
    manual = manual || {};
    const F = [];   // hallazgos
    const add = (sev, area, title, detail, action, sa) => F.push({ sev, area, title, detail, action, sa: sa === undefined ? null : sa });
    const live = snap.live || {};
    const v = live.v || {}, tw = live.twai || {}, al = live.alerts || {};
    const bus = snap.bus || {};
    const dur = snap.dur || 0;
    const active = live.mode === 'active';
    const lost = (tw.missed || 0) + (tw.overrun || 0);   // tramas que la herramienta no pudo leer

    // Equipos (se ignora la propia herramienta)
    const isOwn = (sa) => active && sa === bus.ownAddr;
    const devs = (snap.devices || []).filter((d) => !isOwn(d.sa)).map((d) => {
      const nm = decodeName(d.name);
      const pi = decodePI(d.pi);
      const ci = d.ci ? decodeCI(d.ci) : null;
      return Object.assign({}, d, {
        nm, pi, ci,
        mfrName: nm ? mfrName(nm.mfr) : '-',
        typeName: nm ? fnName(nm.cls, nm.fn) : 'Desconocido',
        label: (pi && pi.model) ? pi.model : (nm ? fnName(nm.cls, nm.fn) : 'Equipo'),
        sev: 'ok',
      });
    });
    const bySa = {}; devs.forEach((d) => { bySa[d.sa] = d; });
    // si hay dos equipos con el mismo nombre, se distinguen por la dirección
    const labelCount = {}; devs.forEach((d) => { labelCount[d.label] = (labelCount[d.label] || 0) + 1; });
    const devLabel = (sa) => { const d = bySa[sa]; return d ? `${d.label} (${d.mfrName}, dir. ${sa})` : `dirección ${sa}`; };
    const shortLabel = (sa) => { const d = bySa[sa]; return !d ? `Equipo en dir. ${sa}` : labelCount[d.label] > 1 ? `${d.label} (dir. ${sa})` : d.label; };
    const pgns = (snap.pgns || []).filter((p) => !isOwn(p.sa));
    const pgnsOf = (sa) => pgns.filter((e) => e.sa === sa);
    // ¿Manda algún mensaje periódico (al menos cada 1,5 s)? Solo en esos equipos
    // un silencio largo significa que se ha caído.
    const periodOf = (sa) => {
      let best = 0;
      // regular: el intervalo mínimo no es mucho menor que el medio (descarta ráfagas, p. ej. AIS)
      pgnsOf(sa).forEach((e) => { if (e.n >= 8 && e.avgI > 0 && e.avgI <= 1500 && e.minI >= e.avgI * 0.25 && (!best || e.avgI < best)) best = e.avgI; });
      return best;
    };

    // ---- 1. Hay tráfico ----
    const others = devs.filter((d) => d.sa !== 254 && d.frames > 0).length;
    if (!bus.frames) {
      add('fail', 'Red', 'No se recibe ningún dato de la red',
        'La herramienta no ve ninguna trama NMEA 2000 durante la prueba.',
        'Comprueba en este orden: 1) que la red tiene alimentación (cable de power con su fusible y el interruptor), 2) que hay dos terminadores, 3) que la herramienta está bien conectada a una T del backbone, 4) que no hay cables CAN-H/CAN-L (blanco/azul) cruzados en algún conector reparado.');
    }

    // ---- 2. Alimentación ----
    if (v.ok) {
      const dev = v.dev !== undefined ? v.dev : null;   // salto brusco máximo (firmware 1.2+)
      const unstable = dev !== null ? dev > 1.0 : (v.max - v.min > 1.5);
      if (v.min < 9.0) add('fail', 'Alimentación', `Tensión de la red demasiado baja (mín. ${fmt(v.min, 2)} V)`,
        `Por debajo de 9 V los equipos NMEA 2000 dejan de funcionar o se reinician. Media ${fmt(v.avg, 2)} V.`,
        'Revisa la batería, el fusible y el cable de alimentación de la red. Busca conectores flojos u oxidados, sobre todo en la T de alimentación. Si la red es larga, aliméntala por el centro.');
      else if (v.min < 10.5) add('warn', 'Alimentación', `Caídas de tensión en la red (mín. ${fmt(v.min, 2)} V)`,
        `La tensión llegó a bajar a ${fmt(v.min, 2)} V (media ${fmt(v.avg, 2)} V). En el extremo más lejano del backbone puede ser aún menor.`,
        'Comprueba la carga de la batería y la conexión de la T de alimentación. Si coincide con el arranque del motor o del motor eléctrico, alimenta la red desde una batería o línea separada.');
      else if (v.avg < 11.5) add('warn', 'Alimentación', `Tensión media baja (${fmt(v.avg, 2)} V)`,
        'La red funciona, pero con poco margen.', 'Carga o revisa la batería que alimenta la red y la caída en el cable de alimentación.');
      if (v.max > 15.5) add('warn', 'Alimentación', `Tensión alta (máx. ${fmt(v.max, 2)} V)`,
        'Por encima de 15,5 V puede haber un problema de regulación de carga.', 'Revisa el regulador del alternador o el cargador de baterías.');
      if (unstable && v.min >= 9.0) add('warn', 'Alimentación', `Tensión inestable (saltos de ${fmt(dev !== null ? dev : v.max - v.min, 1)} V)`,
        `La tensión dio saltos bruscos durante la prueba (entre ${fmt(v.min, 1)} y ${fmt(v.max, 1)} V). Suele ser un conector o cable de alimentación con mal contacto.`,
        'Revisa el cable de alimentación de la red y su conector; mide la tensión moviendo el cable para localizar el mal contacto.');
      if (v.min >= 10.5 && v.avg >= 11.5 && v.max <= 15.5 && !unstable)
        add('ok', 'Alimentación', `Tensión correcta (${fmt(v.min, 1)}–${fmt(v.max, 1)} V)`, '', '');
    } else {
      add('info', 'Alimentación', 'Tensión de la red no medida', 'El sensor de tensión de la herramienta no da lectura.', 'Si quieres medirla, conecta el divisor de tensión al pin 34 (ver guía de montaje).');
    }

    // ---- 3. Terminadores (medida manual) ----
    const T = {
      '60': ['ok', 'Terminadores correctos (aprox. 60 ohm)', '', ''],
      '120': ['fail', 'Falta un terminador (aprox. 120 ohm)', 'Solo hay un terminador. Sin los dos, la señal rebota y aparecen errores y equipos que se caen.', 'Pon un terminador en cada extremo del backbone (dos en total, uno macho y otro hembra).'],
      '40': ['fail', 'Sobran terminadores (40 ohm o menos)', 'Hay tres o más terminadores. Pasa a menudo con equipos que llevan terminador interno o con terminadores olvidados en una T.', 'Deja solo dos terminadores, uno en cada extremo del backbone. Revisa si algún equipo tiene terminación interna activada.'],
      'open': ['fail', 'Sin terminadores o backbone cortado (resistencia muy alta)', 'Entre CAN-H y CAN-L no hay continuidad: no hay terminadores o el cable del backbone está cortado.', 'Pon dos terminadores; si ya están, busca el tramo cortado midiendo por secciones.'],
      'short': ['fail', 'Cortocircuito entre CAN-H y CAN-L (casi 0 ohm)', 'Las líneas de datos están en cortocircuito: la red no puede funcionar.', 'Desconecta equipos y cables uno a uno hasta que desaparezca el corto; suele ser un conector con agua o un cable pisado.'],
    };
    if (T[manual.term]) { const t = T[manual.term]; add(t[0], 'Cableado', t[1], t[2], t[3]); }
    else add('info', 'Cableado', 'Terminadores no medidos', 'No se hizo la medida de resistencia entre CAN-H y CAN-L.', 'Con la red apagada, mide entre el cable blanco y el azul de una T libre: debe dar unos 60 ohm.');

    // ---- 4. Errores CAN ----
    const busErr = tw.busErr || 0;
    const errRate = bus.frames ? busErr / (bus.frames + busErr) * 100 : 0;
    if (al.busOff > 0) add('fail', 'Comunicación', 'La red llegó a bloquearse (bus-off)',
      `Hubo tantos errores que el controlador de la herramienta tuvo que desconectarse (${plural(al.busOff, 'vez', 'veces')}).`,
      'Fallo grave de cableado: revisa terminadores, cortos entre CAN-H/CAN-L, conectores con agua y cables dañados. Desconecta equipos uno a uno para localizar el que causa los errores.');
    if (bus.frames && errRate > 3) add('fail', 'Comunicación', `Muchos errores de comunicación (${fmt(errRate, 1)} %)`,
      `${busErr} errores en ${bus.frames} tramas. Con este nivel los datos llegan mal o se pierden.`,
      'Revisa los dos terminadores, conectores con humedad o corrosión, derivaciones de más de 6 m y cables con la malla dañada. Desconecta equipos uno a uno para ver cuál los provoca.');
    else if (bus.frames && (errRate > 0.3 || (al.errPass > 0 && others > 0))) add('warn', 'Comunicación', `Errores de comunicación (${fmt(errRate, 2)} %)`,
      `${busErr} errores en ${bus.frames} tramas${al.errPass ? '; el controlador de la herramienta llegó a modo "error pasivo"' : ''}. La red funciona pero no está limpia.`,
      'Revisa terminadores y conectores (humedad, pines doblados). Si los errores aparecen al arrancar un motor, revisa masas y la separación del cableado de potencia.');
    else if (bus.frames) add('ok', 'Comunicación', 'Comunicación limpia, sin errores relevantes', busErr ? `${busErr} errores en ${bus.frames} tramas (${fmt(errRate, 2)} %).` : '', '');
    if (lost > 0) add('info', 'Herramienta', 'La herramienta no pudo leer algunas tramas',
      `${lost} tramas se perdieron dentro de la herramienta (no en la red). Algunos contadores pueden quedarse un poco cortos.`, '');

    // ---- 5. Carga del bus ----
    if (bus.loadPeak > 90) add('fail', 'Red', `Red saturada (pico ${fmt(bus.loadPeak, 0)} %)`, 'Con la red tan llena se retrasan y pierden mensajes.', 'Reduce la frecuencia de envío de algunos equipos o divide la red con un puente NMEA 2000.');
    else if (bus.loadAvg > 40 || bus.loadPeak > 70) add('warn', 'Red', `Carga de red alta (media ${fmt(bus.loadAvg, 0)} %, pico ${fmt(bus.loadPeak, 0)} %)`, 'Hay poco margen para añadir equipos.', 'Revisa si hay equipos enviando datos repetidos o innecesarios.');

    // ---- 6. Direcciones ----
    const raw = snap.devices || [];
    const nameAt = {};   // NAME → direcciones donde se ha visto
    raw.forEach((d) => (d.names || []).forEach((n) => { (nameAt[n] = nameAt[n] || []).push(d.sa); }));
    raw.filter((d) => d.sa === 254).forEach((d) => {
      const nm = decodeName(d.name);
      add('fail', 'Direcciones', 'Un equipo no consigue dirección en la red',
        `Un equipo ${nm ? mfrName(nm.mfr) + ' (' + fnName(nm.cls, nm.fn) + ')' : ''} se anuncia con la dirección 254 ("no puedo reclamar dirección"): no podrá enviar datos.`,
        'Reinicia toda la red. Si sigue igual, actualiza el software de ese equipo o retíralo; puede haber demasiados equipos o un conflicto que no se resuelve.', 254);
    });
    raw.filter((d) => d.names && d.names.length > 1 && d.sa !== 254 && !isOwn(d.sa)).forEach((d) => {
      const who = d.names.map((n) => { const x = decodeName(n); return x ? `${mfrName(x.mfr)} ${fnName(x.cls, x.fn)}` : n; }).join(' y ');
      // nameChg: veces que la dirección cambió de dueño. Una sola vez es el
      // reparto normal de direcciones (un equipo cede la suya y se va a otra).
      const changes = d.nameChg !== undefined ? d.nameChg : d.names.length - 1;
      const movedAway = d.names.filter((n) => n !== d.name).every((n) => (nameAt[n] || []).some((sa) => sa !== d.sa));
      if (changes >= 2 && !movedAway) {
        add('fail', 'Direcciones', `Conflicto de dirección (dir. ${d.sa})`,
          `Varios equipos se disputan la misma dirección: ${who}. La dirección cambió de dueño ${changes} veces durante la prueba.`,
          'Reinicia la red con todos los equipos conectados. Si persiste, actualiza el software de los equipos implicados o configura una dirección fija distinta.', d.sa);
      } else {
        add('info', 'Direcciones', `Cambio de dirección resuelto (dir. ${d.sa})`,
          `La dirección ${d.sa} la usaron ${who}. ${movedAway ? 'El otro equipo pasó a otra dirección: es el reparto normal.' : 'Puede ser un equipo que se apagó y otro que ocupó su dirección.'}`, '');
      }
    });

    // ---- 7. Estabilidad de cada equipo ----
    devs.forEach((d) => {
      if (d.sa === 254) return;
      const period = periodOf(d.sa);
      const restarted = d.claimsUnreq > 0 || (d.hb && d.hb.resets > 0);
      const gone = period > 0 && dur - d.last > Math.max(10000, period * 10);
      const label = shortLabel(d.sa);
      // el mismo equipo (mismo NAME) siguió funcionando en otra dirección
      const movedTo = d.name && d.name !== '0000000000000000' && devs.find((o) => o.sa !== d.sa && o.name === d.name && o.last > d.last);
      if (gone && movedTo) {
        add('info', 'Direcciones', `${label} cambió de dirección (de ${d.sa} a ${movedTo.sa})`, 'El equipo pasó a otra dirección durante la prueba y siguió funcionando.', '', d.sa);
      } else if (gone) {
        add('fail', 'Equipos', `${label} desapareció de la red`, `Dejó de enviar datos a los ${secs(d.last)} de la prueba y no volvió.`,
          'Revisa la alimentación del equipo, su cable de derivación y el conector de la T. Si está bien, prueba el equipo en otra T.', d.sa);
      } else if (period > 0 && d.gaps5s > 0) {
        add('warn', 'Equipos', `${label} se desconecta a ratos`,
          `Estuvo ${secs(d.maxGap)} sin comunicar (${plural(d.gaps5s, 'corte', 'cortes')})${restarted ? ' y se reinició' : ''}.`,
          restarted ? 'Se reinicia: revisa su alimentación (fusible, conexiones, tensión de batería) y su cable de derivación. Si el resto de la red es estable, el fallo está en el equipo o su cableado.'
            : 'Revisa el cable de derivación y el conector de la T (pines, humedad). Prueba a moverlo con la red funcionando para ver si se corta.', d.sa);
      } else if (restarted) {
        add('warn', 'Equipos', `${label} se reinició durante la prueba`, 'Volvió a anunciarse en la red tras quedarse callado, sin que nadie se lo pidiera: señal de reinicio.',
          'Suele ser una caída de tensión: revisa su alimentación y la tensión de la red cuando falla.', d.sa);
      }
      // Mensajes largos (fast packet) incompletos: uno suelto en una red grande
      // puede ser casual; se avisa si se repite o si es una proporción apreciable.
      if (d.fpErr > 0) {
        const total = d.fpErr + (d.fpOk || 0);
        const pct = total ? d.fpErr / total * 100 : 100;
        const det = `${d.fpErr === 1 ? 'Un mensaje largo llegó cortado' : d.fpErr + ' mensajes largos llegaron cortados'}${d.fpOk ? ` de ${total} (${fmt(pct, 1)} %)` : ''}.`;
        if (lost > 0) add('info', 'Equipos', `Mensajes incompletos de ${label}`, det + ' La herramienta también perdió tramas, así que puede no ser culpa del equipo.', '', d.sa);
        else if (d.fpErr >= 3 || pct > 2) add('warn', 'Equipos', `Mensajes incompletos de ${label}`, det,
          'Posible ruido o mal contacto en su derivación. Revisa el cable y el conector; si hay errores CAN, revisa también los terminadores.', d.sa);
        else add('info', 'Equipos', `Mensaje incompleto aislado de ${label}`, det + ' Un caso aislado no indica avería.', '', d.sa);
      }
      if (active && !d.pi && d.frames > 2) {
        if (bus.infoOverflow > 0) add('info', 'Equipos', `${label}: información de producto no guardada`, 'La red tiene tantos equipos que la herramienta no pudo guardar la información de todos.', '', d.sa);
        else add('info', 'Equipos', `${label} no envió su información de producto`,
          'No respondió con modelo, software y número de serie.', 'Puede ser un equipo antiguo o una pasarela. Si es moderno, revisa que su software esté actualizado.', d.sa);
      }
    });

    // Dos registros a la vez en el tiempo y de equipos distintos (no el mismo
    // equipo que cambió de dirección)
    const overlap = (a, b) => a.first <= b.last && b.first <= a.last;
    const sameDevice = (sa1, sa2) => { const a = bySa[sa1], b = bySa[sa2]; return a && b && a.name && a.name === b.name && a.name !== '0000000000000000'; };
    const conflicting = (list) => {
      // devuelve las direcciones que coinciden en el tiempo con al menos otra distinta
      const out = new Set();
      for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) {
        const a = list[i], b = list[j];
        if (a.sa === b.sa || sameDevice(a.sa, b.sa) || !overlap(a, b) || a.n < 2 || b.n < 2) continue;
        out.add(a.sa); out.add(b.sa);
      }
      return [...out].sort((x, y) => x - y);
    };

    // ---- 8. Instancias duplicadas ----
    const INST = { 127488: 'motor', 127489: 'motor', 127493: 'transmisión', 127508: 'batería', 127505: 'depósito',
      130312: 'temperatura', 130316: 'temperatura', 130313: 'humedad', 130314: 'presión', 127245: 'timón' };
    const seenDup = {};
    Object.keys(INST).forEach((k) => {
      const p = +k; const g = {};
      pgns.filter((e) => e.pgn === p && e.inst !== 255).forEach((e) => {
        // timón: el piloto manda la orden de ángulo con la posición vacía; eso no es un duplicado
        if (p === 127245) { const b = hexToBytes(e.d); if (b.length >= 6 && s16(b, 4) === 0x7FFF) return; }
        (g[e.inst] = g[e.inst] || []).push(e);
      });
      Object.entries(g).forEach(([inst, list]) => {
        const sas = conflicting(list);
        if (sas.length < 2) return;
        const key = INST[p] + ':' + inst + ':' + sas.join(',');
        if (seenDup[key]) return; seenDup[key] = 1;
        const engine = p === 127488 || p === 127489 || p === 127493;
        add(engine ? 'fail' : 'warn', 'Configuración', `Instancia de ${INST[p]} duplicada (${instText(p, +inst)})`,
          `${sas.map(devLabel).join(' y ')} envían datos de ${INST[p]} con la misma instancia. La pantalla mezclará los datos o mostrará solo uno.`,
          engine ? 'Asigna instancias distintas a cada motor (normalmente babor 0, estribor 1). En los motores lo configura la pasarela o la centralita, a menudo el servicio oficial.'
            : 'Asigna una instancia distinta a cada equipo desde su configuración (en Lowrance: Ajustes > Red > Lista de dispositivos > equipo > Configurar). En equipos de otras marcas a veces no se puede cambiar desde la pantalla.', sas);
      });
    });

    // ---- 9. Varias fuentes del mismo dato ----
    const SRC = { 129025: 'posición GPS', 127250: 'rumbo', 128267: 'profundidad', 130306: 'viento' };
    Object.entries(SRC).forEach(([k, name]) => {
      const g = {};
      pgns.filter((e) => e.pgn === +k).forEach((e) => { (g[e.inst] = g[e.inst] || []).push(e); });
      const sas = [...new Set([].concat(...Object.values(g).map(conflicting)))].sort((a, b) => a - b);
      if (sas.length > 1) add('info', 'Configuración', `Varias fuentes de ${name}`, `${sas.map(devLabel).join(', ')} envían ${name}.`,
        'Es normal, pero elige la fuente preferida en la pantalla (en Lowrance: Ajustes > Red > Fuentes de datos) para que no salte de una a otra.');
    });

    // ---- 10. Regularidad de los mensajes ----
    // Se compara cada mensaje con su PROPIO ritmo: muchos equipos mandan más lento
    // de lo que dice la norma y eso no es una avería; un hueco grande sí lo es.
    pgns.forEach((e) => {
      const def = LK && LK.pgn[e.pgn];
      const d = bySa[e.sa];
      if (!def || !def.i || def.i > 5000 || isProp(e.pgn) || e.n < 10 || !e.avgI) return;
      if ([60928, 126996, 126998, 126993, 59904, 59392, 126464, 126208].includes(e.pgn)) return;
      if (d && (d.gaps5s > 0 || d.claimsUnreq > 0)) return;   // ya avisado como desconexión o reinicio
      if (lost > 0) return;                                   // los huecos pueden ser de la herramienta
      if (e.maxI > Math.max(e.avgI * 6, def.i * 5, 2500)) add('warn', 'Datos', `${pgnName(e.pgn)} de ${shortLabel(e.sa)} tiene cortes`,
        `Normalmente llega cada ${secs(e.avgI)}, pero hubo un hueco de ${secs(e.maxI)}.`, 'Revisa el cable de derivación del equipo y los errores de la red.', e.sa);
      else if (e.avgI > def.i * 4 && e.avgI > 1000) add('info', 'Datos', `${pgnName(e.pgn)} de ${shortLabel(e.sa)} llega más lento de lo habitual`,
        `Llega cada ${secs(e.avgI)}; lo normal es cada ${secs(def.i)}. Suele ser la configuración del equipo, no una avería.`, '', e.sa);
    });

    // ---- 11. Valores de los datos ----
    const lastOf = (p) => pgns.filter((e) => e.pgn === p);
    const rpmKnown = new Set(lastOf(127488).filter((e) => e.na < e.n).map((e) => e.inst));
    lastOf(127489).forEach((e) => {
      const bits = e.or >>> 0;
      const run = e.orRun === undefined ? bits : e.orRun >>> 0;
      ENG_BITS.forEach(([name, sev, offNormal], i) => {
        if (!sev || !(bits & (1 << i))) return;
        const who = `vía ${devLabel(e.sa)}`;
        if (offNormal && !(run & (1 << i))) {
          // solo con el motor parado (o sin saber las rpm): con el contacto dado es normal
          add('info', 'Motor', `Motor ${e.inst}: ${name} (con el motor parado)`,
            `El motor (${who}) marcó "${name}" ${rpmKnown.has(e.inst) ? 'mientras estaba parado' : 'y no se sabe si estaba en marcha'}. Con el contacto dado y el motor parado es normal.`,
            '', e.sa);
          return;
        }
        add(sev, 'Motor', `Motor ${e.inst}: ${name}`, `El motor (${who}) ha activado la alarma "${name}" durante la prueba.`,
          'Es una alarma del propio motor, no de la red: revísalo con la herramienta del fabricante del motor o su servicio oficial.', e.sa);
      });
    });
    lastOf(127508).forEach((e) => {
      const b = hexToBytes(e.d); if (b.length < 3) return;
      const raw16 = s16(b, 1); if (raw16 === 0x7FFF) return;
      const volt = raw16 / 100;
      if (volt < 5) return;   // sin lectura o entrada sin usar
      // tensión nominal del banco (12, 24, 36 o 48 V: los motores eléctricos van a 24/36 V)
      const nom = volt < 17 ? 12 : volt < 32 ? 24 : volt < 46 ? 36 : 48;
      const low = nom * 11.8 / 12, high = nom * 15 / 12;
      const bName = `Batería ${e.inst}${nom !== 12 ? ` (${nom} V)` : ''}`;
      if (volt < low) add('warn', 'Baterías', `${bName} baja (${fmt(volt, 2)} V)`, `Lo indica ${devLabel(e.sa)}.`, 'Carga la batería y comprueba su estado; con la batería baja pueden aparecer fallos en la red.', e.sa);
      else if (volt > high) add('warn', 'Baterías', `${bName} con tensión alta (${fmt(volt, 2)} V)`, `Lo indica ${devLabel(e.sa)}.`, 'Revisa el regulador o el cargador.', e.sa);
    });
    // Dato vacío siempre o a ratos
    const naCheck = (pgn, area, noneTitle, noneDetail, noneAction, someTitle, someAction) => {
      lastOf(pgn).forEach((e) => {
        if (!e.n || !e.na) return;
        if (e.na >= e.n) add('warn', area, `${devLabel(e.sa)} ${noneTitle}`, noneDetail, noneAction, e.sa);
        else if (e.n >= 10 && e.na / e.n > 0.1) add('warn', area, `${shortLabel(e.sa)} ${someTitle}`,
          `El ${fmt(e.na / e.n * 100, 0)} % de los mensajes llegaron vacíos.`, someAction, e.sa);
      });
    };
    naCheck(128267, 'Sondas', 'no da profundidad', 'Envía el mensaje de profundidad pero siempre vacío.',
      'Normal si el transductor está fuera del agua. Si está en el agua: revisa el cable y el conector del transductor y que no tenga suciedad o pintura.',
      'pierde la profundidad a ratos', 'Normal en aguas muy someras, muy profundas o con burbujas. Si no, revisa la instalación y el cable del transductor.');
    naCheck(129025, 'GPS', 'sin posición', 'El GPS está conectado pero no da posición.',
      'Comprueba que tenga cielo despejado. Si sigue sin posición en exterior varios minutos, revisa antena y cable.',
      'pierde la posición a ratos', 'Revisa la ubicación de la antena (sin obstáculos ni metal cerca) y su cable.');
    naCheck(127250, 'Rumbo', 'no da rumbo', 'Envía rumbo vacío.', 'Revisa la calibración del compás o del sensor de rumbo.',
      'pierde el rumbo a ratos', 'Revisa la instalación del compás (lejos de metales y cables de potencia) y su calibración.');
    lastOf(129029).forEach((e) => {
      const b = hexToBytes(e.d); if (b.length < 36) return;
      const method = b[31] >> 4, svs = b[33], hdopRaw = s16(b, 34);
      const hdop = hdopRaw === 0x7FFF ? null : hdopRaw / 100;
      if (method === 0) add('warn', 'GPS', `${devLabel(e.sa)} sin fix GPS`, `Satélites: ${svs === 255 ? '-' : svs}.`, 'Ponlo con cielo despejado; si no consigue fix, revisa la antena.', e.sa);
      else if (method <= 5 && ((svs !== 255 && svs < 5) || (hdop !== null && hdop > 3))) add('warn', 'GPS', `${devLabel(e.sa)} con señal GPS pobre`,
        `${svs === 255 ? '-' : svs} satélites, HDOP ${hdop === null ? '-' : fmt(hdop, 1)}.`, 'Normal en interiores. En exterior, revisa la ubicación de la antena (sin obstáculos ni metal cerca).', e.sa);
    });

    // ---- 12. Consumo (LEN) ----
    const lenTotal = devs.reduce((s, d) => s + (d.pi && d.pi.len ? d.pi.len : 0), 0);
    if (lenTotal > 50) add('warn', 'Alimentación', `Consumo de red alto (${lenTotal} LEN = ${fmt(lenTotal * 0.05, 2)} A)`,
      'La suma de consumos declarados por los equipos es alta.', 'Comprueba la tensión en el extremo más lejano del backbone y alimenta la red por el centro si cae mucho.');

    // ---- 13. Límites de la herramienta ----
    if (bus.pgOverflow > 0) add('info', 'Herramienta', 'Red muy grande: no se han registrado todos los mensajes',
      'La tabla de mensajes de la herramienta se llenó; el análisis de algunos mensajes puede estar incompleto.', '');

    // ---- Estado por equipo ----
    const rank = { ok: 0, info: 0, warn: 1, fail: 2 };
    F.forEach((f) => {
      const list = Array.isArray(f.sa) ? f.sa : (f.sa === null ? [] : [f.sa]);
      list.forEach((sa) => { if (bySa[sa] && rank[f.sev] > rank[bySa[sa].sev]) bySa[sa].sev = f.sev; });
    });

    const order = { fail: 0, warn: 1, info: 2, ok: 3 };
    F.forEach((f, i) => { f.i = i; });
    F.sort((a, b) => order[a.sev] - order[b.sev] || a.i - b.i);
    F.forEach((f) => { delete f.i; });
    const nFail = F.filter((f) => f.sev === 'fail').length, nWarn = F.filter((f) => f.sev === 'warn').length;
    const verdict = nFail ? 'fail' : nWarn ? 'warn' : 'ok';
    const actions = [];
    F.filter((f) => (f.sev === 'fail' || f.sev === 'warn') && f.action).forEach((f) => {
      const same = actions.find((a) => a.text === f.action);
      if (same) { if (!same.more) same.more = []; same.more.push(f.title); } else actions.push({ sev: f.sev, title: f.title, text: f.action });
    });
    actions.forEach((a) => { if (a.more) { a.title += ` (y ${plural(a.more.length, 'caso más', 'casos más')})`; } });

    const pgnRows = pgns.slice().sort((a, b) => a.sa - b.sa || a.pgn - b.pgn || a.inst - b.inst).map((e) => ({
      sa: e.sa, dev: bySa[e.sa] ? bySa[e.sa].label : '-', pgn: e.pgn,
      name: pgnName(e.pgn, bySa[e.sa] && bySa[e.sa].mfrName) + (instText(e.pgn, e.inst) ? ` [${instText(e.pgn, e.inst)}]` : ''),
      n: e.n, every: e.n > 1 ? secs(e.avgI) : '-', expected: (LK && LK.pgn[e.pgn] && LK.pgn[e.pgn].i && !isProp(e.pgn)) ? secs(LK.pgn[e.pgn].i) : '-',
    }));

    return {
      verdict, nFail, nWarn, findings: F, actions, devices: devs.sort((a, b) => a.sa - b.sa), pgnRows,
      metrics: { dur, frames: bus.frames, loadAvg: bus.loadAvg, loadPeak: bus.loadPeak, errRate, busErr,
        v, nDev: devs.filter((d) => d.sa !== 254).length, lenTotal, mode: live.mode, term: manual.term || 'skip', fw: live.fw || '' },
      ref: manual.ref || '', date: manual.date || new Date(),
    };
  }

  const API = { analyze, decodeName, decodePI, decodeCI, fmt, secs, pgnName };
  if (typeof module !== 'undefined') module.exports = API; else root.N2K = API;
})(typeof window !== 'undefined' ? window : globalThis);
