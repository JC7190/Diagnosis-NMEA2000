// Interfaz: pantallas, sondeo del ESP32, diagnóstico guiado y PDF
(function () {
  const $ = (s) => document.querySelector(s);
  const $$ = (s) => [...document.querySelectorAll(s)];
  const fmt = N2K.fmt;
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const S = { dur: 60, mode: 'active', snap: null, result: null, timer: null, gen: 0, pollMs: 0, failCount: 0, finishing: false, first: true, pdfUrl: null };

  function show(id) {
    $$('.screen').forEach((s) => s.classList.toggle('active', s.id === id));
    window.scrollTo(0, 0);
  }
  let toastT = null;
  function toast(t) { const e = $('#toast'); e.textContent = t; e.classList.add('show'); clearTimeout(toastT); toastT = setTimeout(() => e.classList.remove('show'), 3200); }

  // Petición con tiempo límite (si la Wi-Fi se corta, no se queda colgada)
  // ---------- captura de tramas (CSV) ----------
  // Durante la prueba se descargan las tramas del ESP32 cada 0,5 s y se guardan aquí.
  const CAP = { lines: [], next: 0, lost: 0, on: false, timer: null, busy: false };
  async function capFetch() {
    if (CAP.busy) return 0;
    CAP.busy = true;
    try {
      const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
      const to = ctl ? setTimeout(() => ctl.abort(), 8000) : null;
      const r = await fetch('/api/frames?from=' + CAP.next, { cache: 'no-store', signal: ctl ? ctl.signal : undefined });
      if (to) clearTimeout(to);
      if (!r.ok) return 0;
      const t = await r.text();
      const nl = t.indexOf('\n');
      const m = /#seq=(\d+);lost=(\d+)/.exec(t.slice(0, nl));
      if (!m) return 0;
      const lost = +m[2];
      if (lost) { CAP.lost += lost; CAP.lines.push(`;;;;HUECO ${lost} tramas no descargadas`); }
      const body = t.slice(nl + 1);
      let got = 0;
      if (body) { const ls = body.split('\n'); for (const l of ls) if (l) { CAP.lines.push(l); got++; } }
      CAP.next = +m[1];
      return got;
    } catch (e) { return 0; } finally { CAP.busy = false; }
  }
  function capStart(reset) {
    if (reset) { CAP.lines = []; CAP.next = 0; CAP.lost = 0; }
    CAP.on = true; clearInterval(CAP.timer); CAP.timer = setInterval(capFetch, 500);
  }
  async function capStop() {
    clearInterval(CAP.timer); CAP.timer = null;
    if (!CAP.on) return;
    CAP.on = false;
    for (let i = 0; i < 20; i++) { const g = await capFetch(); if (g < 1200) break; }   // lo que quede
  }
  function capCsv(r) {
    const d = r.date instanceof Date ? r.date : new Date();
    const head = [`# PoweryMax Nmea2000 Diagnostic v${r.metrics.fw || ''} - captura de tramas`,
      `# fecha=${d.toISOString()};referencia=${(r.ref || '').replace(/[;\n]/g, ' ')};duracion_s=${Math.round(r.metrics.dur / 1000)};modo=${r.metrics.mode};tramas=${CAP.lines.length};no_descargadas=${CAP.lost}`,
      'marca_tiempo_us;id29_hex;dlc;datos_hex;flags'];
    return head.concat(CAP.lines).join('\n') + '\n';
  }

  async function api(path, ms) {
    const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const to = ctl ? setTimeout(() => ctl.abort(), ms || 10000) : null;
    try {
      const r = await fetch(path, { cache: 'no-store', signal: ctl ? ctl.signal : undefined });
      if (!r.ok) throw new Error(r.status);
      return await r.json();
    } finally { if (to) clearTimeout(to); }
  }

  // ---------- sondeo ----------
  // Se pide la versión ligera (sin tabla de mensajes) y no se lanza la siguiente
  // petición hasta que acaba la anterior: no se acumulan en el ESP32.
  async function poll() {
    clearTimeout(S.timer); S.timer = null;
    const gen = S.gen;
    try {
      const s = await api('/api/snapshot?lite=1', 8000);
      if (gen !== S.gen) return;   // se ha cambiado de pantalla mientras tanto
      S.snap = s; S.failCount = 0;
      $('#conn').className = 'pill on'; $('#conn').textContent = 'Conectado';
      if (S.first) { S.first = false; restore(s); }
      render(s);
    } catch (e) {
      if (gen !== S.gen) return;
      if (++S.failCount > 1) {
        $('#conn').className = 'pill off'; $('#conn').textContent = 'Sin conexión';
        setStatus('bad', 'Sin conexión con la herramienta', 'Comprueba que el iPhone sigue conectado a la Wi-Fi «PoweryMax Nmea2000 Diagnostic» y que la herramienta tiene alimentación.');
      }
    }
    if (gen === S.gen && S.pollMs && !document.hidden) S.timer = setTimeout(poll, S.pollMs);
  }
  function startPolling(ms) { S.gen++; S.pollMs = ms; clearTimeout(S.timer); S.timer = null; poll(); }
  function stopPolling() { S.gen++; S.pollMs = 0; clearTimeout(S.timer); S.timer = null; }
  // en segundo plano no se pregunta nada (ahorra batería); al volver, se sigue
  document.addEventListener('visibilitychange', () => { if (!document.hidden && S.pollMs && !S.timer) poll(); });

  // Si se recarga la página a mitad de una prueba, se vuelve a la pantalla que toca
  function restore(s) {
    const d = (s.live || {}).diag || {};
    if (d.running) { S.finishing = false; S.startedAt = new Date(Date.now() - (d.elapsed || 0)); show('running'); S.pollMs = 1000; capStart(true); }
    else if (d.done && $('#home').classList.contains('active')) { S.finishing = true; stopPolling(); show('term'); }
  }

  function setStatus(cls, t, d) {
    $('#status').className = 'status ' + cls;
    $('#stT').textContent = t; $('#stD').textContent = d;
  }
  // Estado de la conexión explicado en palabras sencillas (pantalla de inicio)
  function homeStatus(s, nDev, er) {
    const L = s.live || {}, b = s.bus || {}, v = L.v || {};
    const hasV = v.now > 4;
    if (!b.frames) {
      if ((s.dur || 0) < 4000) return setStatus('wait', 'Escuchando la red…', 'Un momento, buscando equipos.');
      if (!hasV) return setStatus('bad', 'No llega nada de la red', 'Comprueba que la herramienta está conectada a una T del backbone y que la red NMEA 2000 tiene alimentación (interruptor y fusible).');
      return setStatus('warn', 'Hay tensión, pero no llegan datos', 'Enciende algún equipo de la red (pantalla, GPS…). Si ya están encendidos, haz el diagnóstico para buscar la causa.');
    }
    if (hasV && v.now > CFG.tension.seguridad24V) return setStatus('bad', `¡Tensión peligrosa: ${fmt(v.now, 1)} V!`, 'Más de 16 V en la red: puede estar a 24 V. Desconecta la alimentación de la red.');
    if (hasV && v.now < CFG.tension.avisoBajo) return setStatus('warn', `Red detectada con tensión baja (${fmt(v.now, 1)} V)`, 'Puedes hacer el diagnóstico; el informe te dirá qué revisar.');
    if (er > 3) return setStatus('warn', 'Red detectada con muchos errores', 'Haz el diagnóstico para ver la causa.');
    return setStatus('ok', `Red detectada: ${nDev} equipo${nDev === 1 ? '' : 's'}`, 'Todo listo. Pulsa «Iniciar diagnóstico completo».');
  }

  function render(s) {
    const L = s.live || {}, b = s.bus || {}, tw = L.twai || {}, d = L.diag || {};
    const v = L.v || {};
    if (L.fw) $('#fwv').textContent = 'v' + L.fw;
    const devs = (s.devices || []).filter((x) => !(x.sa === b.ownAddr && L.mode === 'active'));
    // inicio
    const tile = (id, val, cls) => { const el = $(id); el.textContent = val; el.parentElement.classList.remove('bad', 'mid', 'good'); if (cls) el.parentElement.classList.add(cls); };
    const TV = CFG.tension;
    tile('#tV', v.now > 4 ? fmt(v.now, 1) + ' V' : '—', v.now > 4 ? (v.now < TV.falloBajo || v.now > TV.falloAlto ? 'bad' : v.now < TV.avisoBajo ? 'mid' : 'good') : null);
    $('#tVs').textContent = v.ok ? `mín ${fmt(v.min, 1)} · máx ${fmt(v.max, 1)}` : 'sin lectura';
    tile('#tLoad', fmt(b.loadNow, 0) + ' %', b.loadNow >= CFG.carga.fallo ? 'bad' : b.loadNow >= CFG.carga.aviso ? 'mid' : null);
    $('#tLoads').textContent = `pico ${fmt(b.loadPeak, 0)} %`;
    const nDev = devs.filter((x) => x.sa !== 254).length;
    tile('#tDev', String(nDev));
    const busErr = tw.busErr || 0;
    const er = b.frames ? busErr / (b.frames + busErr) * 100 : 0;
    homeStatus(s, nDev, er);
    const eps = s.dur > 0 ? busErr / (s.dur / 1000) : 0;
    tile('#tErr', String(busErr), eps > CFG.errores.falloPorSegundo ? 'bad' : busErr > 0 ? 'mid' : b.frames ? 'good' : null);
    $('#tErrs').textContent = b.frames ? `${fmt(er, 2)} % · ${tw.state || '-'}` : `CAN ${tw.state || '-'}`;
    const list = $('#liveDevs');
    let html;
    if (!devs.length) html = `<div class="empty">${b.frames ? 'Recibiendo tramas…' : 'Esperando datos de la red…'}</div>`;
    else html = devs.map((x) => {
      const nm = N2K.decodeName(x.name), pi = N2K.decodePI(x.pi);
      const typical = x.frames > 1 ? (x.last - x.first) / (x.frames - 1) : 0;
      // callado = lleva sin hablar bastante más que su silencio habitual
      const silent = s.dur - x.last > Math.max(5000, typical * 3 + 1000, (x.maxGap || 0) * 2 + 1000);
      const mfr = nm ? (LK.mfr[nm.mfr] || 'Fabricante ' + nm.mfr) : 'Sin identificar';
      const title = pi && pi.model ? pi.model : mfr;
      return `<div class="dev"><div class="ic">${x.sa}</div><div class="tx"><b>${esc(title)}</b>
        <small>${esc(mfr)}${pi && pi.sw ? ' · SW ' + esc(pi.sw) : ''}</small></div>
        <div class="st ${silent ? 'warn' : 'ok'}">${silent ? 'Callado' : 'Activo'}</div></div>`;
    }).join('');
    if (list.dataset.h !== html) { list.innerHTML = html; list.dataset.h = html; }   // sin repintar si no cambia

    // en curso
    if ($('#running').classList.contains('active')) {
      const pct = d.duration ? Math.min(100, d.elapsed / d.duration * 100) : 0;
      $('#ringBar').style.strokeDashoffset = 326.7 * (1 - pct / 100);
      $('#ringPct').textContent = Math.round(pct) + ' %';
      $('#ringLeft').textContent = `quedan ${Math.max(0, Math.ceil((d.duration - d.elapsed) / 1000))} s`;
      const el = d.elapsed / 1000;
      $('#phase').textContent = L.mode === 'active' && el < 3 ? 'Preguntando a los equipos quién es quién…'
        : L.mode === 'active' && el < 22 ? 'Leyendo modelo, software y nº de serie…'
        : 'Vigilando estabilidad, errores y tensión…';
      $('#rDev').textContent = devs.filter((x) => x.sa !== 254).length; $('#rFr').textContent = b.frames || 0; $('#rErr').textContent = busErr;
      if (d.done && !S.finishing) { S.finishing = true; stopPolling(); capStop(); show('term'); }
    }
  }

  // ---------- flujo ----------
  $('#bSetup').onclick = () => show('setup');
  $$('[data-go]').forEach((b) => b.onclick = () => show(b.dataset.go));
  $$('#segDur button').forEach((b) => b.onclick = () => { $$('#segDur button').forEach((x) => x.classList.remove('on')); b.classList.add('on'); S.dur = +b.dataset.v; });
  $$('#optMode button').forEach((b) => b.onclick = () => { $$('#optMode button').forEach((x) => x.classList.remove('on')); b.classList.add('on'); S.mode = b.dataset.v; });

  $('#bStart').onclick = async () => {
    const btn = $('#bStart');
    if (btn.disabled) return;
    btn.disabled = true;
    stopPolling();
    try {
      await api(`/api/start?dur=${S.dur}&mode=${S.mode}`);
      S.finishing = false; S.startedAt = new Date();
      $('#ringBar').style.strokeDashoffset = 326.7;
      $('#ringPct').textContent = '0 %'; $('#ringLeft').innerHTML = '&nbsp;';
      $('#rDev').textContent = '0'; $('#rFr').textContent = '0'; $('#rErr').textContent = '0';
      show('running'); startPolling(1000); capStart(true);
    } catch (e) { toast('No se pudo iniciar. ¿Estás conectado a la Wi-Fi «PoweryMax Nmea2000 Diagnostic»?'); startPolling(2000); }
    btn.disabled = false;
  };

  $$('#ansTerm button').forEach((b) => b.onclick = async () => {
    const btns = $$('#ansTerm button');
    if (btns.some((x) => x.disabled)) return;
    btns.forEach((x) => { x.disabled = true; });
    b.classList.add('busy');
    // datos completos (con la tabla de mensajes) solo una vez, al final
    let snap = null;
    for (let i = 0; i < 3 && !snap; i++) { try { snap = await api('/api/snapshot', 20000); } catch (e) { /* reintenta */ } }
    btns.forEach((x) => { x.disabled = false; });
    b.classList.remove('busy');
    if (!snap) { toast('No se pudieron leer los resultados. Comprueba la Wi-Fi y vuelve a tocar la respuesta.'); return; }
    S.snap = snap; S.term = b.dataset.v;
    // solo se pregunta por el transductor si alguna sonda no da profundidad
    if (N2K.needsWaterQuestion(snap)) show('water'); else finish();
  });
  $$('#ansWater button').forEach((b) => b.onclick = () => finish(b.dataset.v));

  function finish(water) {
    S.result = N2K.analyze(S.snap, { term: S.term, water, ref: $('#ref').value.trim(), date: S.startedAt || new Date() });
    renderResult(S.result); show('result');
  }

  $('#bCancel').onclick = async () => {
    if (!confirm('¿Cancelar la prueba? Se perderán los datos recogidos.')) return;
    stopPolling();
    try { await api('/api/resume'); } catch (e) { /* se reintenta al sondear */ }
    S.finishing = true; capStop(); CAP.lines = []; show('home'); startPolling(2000);
  };

  $('#bNew').onclick = async () => {
    try { await api('/api/resume'); } catch (e) { /* se reintenta al sondear */ }
    if (S.pdfUrl) { URL.revokeObjectURL(S.pdfUrl); S.pdfUrl = null; }
    $('#pdfLink').hidden = true; S.result = null; S.finishing = false; CAP.lines = [];
    show('home'); startPolling(2000);
  };

  // ---------- resultado ----------
  const SEV = { fail: 'AVERÍA', warn: 'REVISAR', ok: 'OK', info: 'INFO', na: 'NO MEDIDO' };
  function renderResult(r) {
    const V = { fail: ['Avería detectada', 'Hay fallos que impiden que la red funcione bien.'],
      warn: ['Revisar', 'La red funciona, pero hay puntos que corregir.'], ok: ['Red correcta', 'No se ha encontrado ningún problema.'] }[r.verdict];
    $('#verdict').className = 'verdict ' + r.verdict;
    $('#verdict').innerHTML = `<h1>${V[0]}</h1><p>${V[1]}</p><span class="cnt">${r.nFail} avería${r.nFail === 1 ? '' : 's'} · ${r.nWarn} aviso${r.nWarn === 1 ? '' : 's'}</span>`;
    const m = r.metrics, v = m.v || {};
    const TT = { '60': 'Correctos', '120': 'Falta uno', '40': 'Sobran', open: 'Abierto', short: 'Corto', jump: 'Inestable', skip: 'No medido' };
    $('#resTiles').innerHTML = [
      ['Equipos', m.nDev, ''], ['Tensión', v.ok ? `${fmt(v.min, 1)}–${fmt(v.max, 1)} V` : '—', v.ok ? `media ${fmt(v.avg, 1)} V` : 'no medida'],
      ['Errores CAN', fmt(m.errRate, 2) + ' %', `${m.busErr} errores`], ['Terminadores', TT[m.term], ''],
    ].map((t) => `<div class="tile"><span>${t[0]}</span><b>${esc(t[1])}</b><small>${esc(t[2]) || '&nbsp;'}</small></div>`).join('');
    $('#actions').innerHTML = r.actions.length ? r.actions.map((a, i) =>
      `<div class="act"><div class="n ${a.sev}">${i + 1}</div><div><b>${esc(a.title)}</b><p>${esc(a.text)}</p></div></div>`).join('')
      : '<div class="empty">Nada que corregir 👍</div>';
    $('#findings').innerHTML = r.findings.map((f) =>
      `<div class="fd ${f.sev}"><b><span class="tag st ${f.sev}">${SEV[f.sev]}</span>${esc(f.title)}</b>${f.detail ? `<small>${esc(f.detail)}</small>` : ''}</div>`).join('');
    $('#bCsv').hidden = !CAP.lines.length;
    $('#resDevs').innerHTML = r.devices.map((d) =>
      `<div class="dev"><div class="ic">${d.sa}</div><div class="tx"><b>${esc(d.label)}</b><small>${esc(d.mfrName)} · ${esc(d.typeName)}${d.pi && d.pi.sw ? ' · SW ' + esc(d.pi.sw) : ''}${d.pi && d.pi.serial ? ' · S/N ' + esc(d.pi.serial) : ''}</small></div><div class="st ${d.sev}">${SEV[d.sev]}</div></div>`).join('');
  }

  $('#bPdf').onclick = () => {
    const r = S.result; if (!r) return;
    const go = () => {
      try {
        const doc = N2KReport.build(r, window.jspdf.jsPDF);
        const d = r.date instanceof Date ? r.date : new Date();
        const pad = (n) => String(n).padStart(2, '0');
        const ref = (r.ref || '').normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^\w\- ]+/g, '').trim().replace(/\s+/g, '_').slice(0, 40);
        const name = `Diagnostico_N2K_${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${ref ? '_' + ref : ''}.pdf`;
        doc.save(name);
        // enlace de respaldo (en algunos iPhone la descarga directa no aparece)
        if (S.pdfUrl) URL.revokeObjectURL(S.pdfUrl);
        S.pdfUrl = URL.createObjectURL(doc.output('blob'));
        const a = $('#pdfLink'); a.href = S.pdfUrl; a.download = name; a.hidden = false;
      } catch (e) { toast('Error al generar el PDF'); console.error(e); }
    };
    if (window.jspdf && window.jspdf.jsPDF && window.jspdf.jsPDF.API.autoTable && window.BRAND) return go();
    // carga perezosa de las librerías del PDF (solo la primera vez)
    const load = (src) => new Promise((ok, ko) => { const s = document.createElement('script'); s.src = src; s.onload = ok; s.onerror = () => { s.remove(); ko(); }; document.head.appendChild(s); });
    const btn = $('#bPdf'); btn.disabled = true;
    toast('Preparando el PDF…');
    const jspdf = window.jspdf ? Promise.resolve() : load('jspdf.js');
    jspdf.then(() => (window.jspdf.jsPDF.API.autoTable ? null : load('autotable.js'))).then(() => (window.BRAND ? null : load('brand.js').catch(() => null))).then(go)
      .catch(() => toast('No se pudo cargar el generador de PDF. Vuelve a intentarlo.'))
      .finally(() => { btn.disabled = false; });
  };

  $('#bCsv').onclick = (ev) => {
    ev.preventDefault();
    const r = S.result; if (!r || !CAP.lines.length) return toast('No hay tramas capturadas en esta prueba.');
    const d = r.date instanceof Date ? r.date : new Date();
    const pad = (n) => String(n).padStart(2, '0');
    const name = `Captura_N2K_${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}.csv`;
    const url = URL.createObjectURL(new Blob([capCsv(r)], { type: 'text/csv' }));
    const a = document.createElement('a'); a.href = url; a.download = name; document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
  };

  startPolling(2000);
})();
