// Interfaz: pantallas, sondeo del ESP32, diagnóstico guiado y PDF
(function () {
  const $ = (s) => document.querySelector(s);
  const $$ = (s) => [...document.querySelectorAll(s)];
  const fmt = N2K.fmt;
  const esc = (s) => String(s == null ? '' : s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const S = { dur: 60, mode: 'active', snap: null, result: null, timer: null, failCount: 0, finishing: false };

  function show(id) {
    $$('.screen').forEach((s) => s.classList.toggle('active', s.id === id));
    window.scrollTo(0, 0);
  }
  function toast(t) { const e = $('#toast'); e.textContent = t; e.classList.add('show'); setTimeout(() => e.classList.remove('show'), 2600); }

  async function api(path) {
    const r = await fetch(path, { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    return r.json();
  }

  // ---------- sondeo ----------
  async function poll() {
    try {
      const s = await api('/api/snapshot');
      S.snap = s; S.failCount = 0;
      $('#conn').className = 'pill on'; $('#conn').textContent = 'Conectado';
      render(s);
    } catch (e) {
      if (++S.failCount > 1) { $('#conn').className = 'pill off'; $('#conn').textContent = 'Sin conexión'; }
    }
  }
  function startPolling(ms) { clearInterval(S.timer); S.timer = setInterval(poll, ms); poll(); }

  function render(s) {
    const L = s.live || {}, b = s.bus || {}, tw = L.twai || {}, d = L.diag || {};
    const v = L.v || {};
    if (L.fw) $('#fwv').textContent = 'v' + L.fw;
    const devs = (s.devices || []).filter((x) => !(x.sa === b.ownAddr && L.mode === 'active'));
    // inicio
    const tile = (id, val, cls) => { const el = $(id); el.textContent = val; el.parentElement.classList.remove('bad', 'mid', 'good'); if (cls) el.parentElement.classList.add(cls); };
    tile('#tV', v.now > 4 ? fmt(v.now, 1) + ' V' : '—', v.now > 4 ? (v.now < 10.5 ? 'bad' : v.now < 11.5 ? 'mid' : 'good') : null);
    $('#tVs').textContent = v.ok ? `mín ${fmt(v.min, 1)} · máx ${fmt(v.max, 1)}` : 'sin sensor';
    tile('#tLoad', fmt(b.loadNow, 0) + ' %', b.loadNow > 70 ? 'bad' : b.loadNow > 40 ? 'mid' : null);
    $('#tLoads').textContent = `pico ${fmt(b.loadPeak, 0)} %`;
    tile('#tDev', String(devs.length));
    const er = b.frames ? (tw.busErr || 0) / b.frames * 100 : 0;
    tile('#tErr', String(tw.busErr || 0), er > 3 ? 'bad' : er > 0.3 ? 'mid' : b.frames ? 'good' : null);
    $('#tErrs').textContent = `CAN ${tw.state || '-'}`;
    const list = $('#liveDevs');
    if (!devs.length) list.innerHTML = `<div class="empty">${b.frames ? 'Recibiendo tramas…' : 'Esperando datos de la red…'}</div>`;
    else list.innerHTML = devs.map((x) => {
      const nm = N2K.decodeName(x.name), pi = N2K.decodePI(x.pi);
      const typical = x.frames > 1 ? (x.last - x.first) / (x.frames - 1) : 0;
      const silent = s.dur - x.last > Math.max(5000, typical * 3 + 1000);
      const mfr = nm ? (LK.mfr[nm.mfr] || 'Fabricante ' + nm.mfr) : '';
      return `<div class="dev"><div class="ic">${x.sa}</div><div class="tx"><b>${esc(pi ? pi.model : mfr)}</b>
        <small>${esc(mfr)}${pi && pi.sw ? ' · SW ' + esc(pi.sw) : ''}</small></div>
        <div class="st ${silent ? 'warn' : 'ok'}">${silent ? 'Callado' : 'Activo'}</div></div>`;
    }).join('');

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
      $('#rDev').textContent = devs.length; $('#rFr').textContent = b.frames; $('#rErr').textContent = tw.busErr || 0;
      if (d.done && !S.finishing) { S.finishing = true; clearInterval(S.timer); show('term'); }
    }
  }

  // ---------- flujo ----------
  $('#bSetup').onclick = () => show('setup');
  $$('[data-go]').forEach((b) => b.onclick = () => show(b.dataset.go));
  $$('#segDur button').forEach((b) => b.onclick = () => { $$('#segDur button').forEach((x) => x.classList.remove('on')); b.classList.add('on'); S.dur = +b.dataset.v; });
  $$('#optMode button').forEach((b) => b.onclick = () => { $$('#optMode button').forEach((x) => x.classList.remove('on')); b.classList.add('on'); S.mode = b.dataset.v; });

  $('#bStart').onclick = async () => {
    try {
      $('#bStart').disabled = true;
      await api(`/api/start?dur=${S.dur}&mode=${S.mode}`);
      S.finishing = false; S.startedAt = new Date();
      $('#ringBar').style.strokeDashoffset = 326.7;
      show('running'); startPolling(1000);
    } catch (e) { toast('No se pudo iniciar. ¿Estás conectado a la Wi-Fi N2K-Diagnostico?'); }
    $('#bStart').disabled = false;
  };

  $$('#ansTerm button').forEach((b) => b.onclick = async () => {
    try { S.snap = await api('/api/snapshot'); } catch (e) { /* usa el último */ }
    S.result = N2K.analyze(S.snap, { term: b.dataset.v, ref: $('#ref').value.trim(), date: S.startedAt || new Date() });
    renderResult(S.result); show('result');
  });

  $('#bNew').onclick = async () => {
    try { await api('/api/resume'); } catch (e) { }
    $('#pdfLink').hidden = true; show('home'); startPolling(2000);
  };

  // ---------- resultado ----------
  const SEV = { fail: 'AVERÍA', warn: 'REVISAR', ok: 'OK', info: 'INFO' };
  function renderResult(r) {
    const V = { fail: ['Avería detectada', 'Hay fallos que impiden que la red funcione bien.'],
      warn: ['Revisar', 'La red funciona, pero hay puntos que corregir.'], ok: ['Red correcta', 'No se ha encontrado ningún problema.'] }[r.verdict];
    $('#verdict').className = 'verdict ' + r.verdict;
    $('#verdict').innerHTML = `<h1>${V[0]}</h1><p>${V[1]}</p><span class="cnt">${r.nFail} avería${r.nFail === 1 ? '' : 's'} · ${r.nWarn} aviso${r.nWarn === 1 ? '' : 's'}</span>`;
    const m = r.metrics, v = m.v || {};
    const TT = { '60': 'Correctos', '120': 'Falta uno', '40': 'Sobran', open: 'Abierto', short: 'Corto', skip: 'No medido' };
    $('#resTiles').innerHTML = [
      ['Equipos', m.nDev, ''], ['Tensión', v.ok ? `${fmt(v.min, 1)}–${fmt(v.max, 1)} V` : '—', v.ok ? `media ${fmt(v.avg, 1)} V` : 'no medida'],
      ['Errores CAN', fmt(m.errRate, 2) + ' %', `${m.busErr} errores`], ['Terminadores', TT[m.term], ''],
    ].map((t) => `<div class="tile"><span>${t[0]}</span><b>${esc(t[1])}</b><small>${esc(t[2]) || '&nbsp;'}</small></div>`).join('');
    $('#actions').innerHTML = r.actions.length ? r.actions.map((a, i) =>
      `<div class="act"><div class="n ${a.sev}">${i + 1}</div><div><b>${esc(a.title)}</b><p>${esc(a.text)}</p></div></div>`).join('')
      : '<div class="empty">Nada que corregir 👍</div>';
    $('#findings').innerHTML = r.findings.map((f) =>
      `<div class="fd ${f.sev}"><b><span class="tag st ${f.sev}">${SEV[f.sev]}</span>${esc(f.title)}</b>${f.detail ? `<small>${esc(f.detail)}</small>` : ''}</div>`).join('');
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
        const ref = (r.ref || '').replace(/[^\w\- ]+/g, '').trim().replace(/\s+/g, '_');
        const name = `Diagnostico_N2K_${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${ref ? '_' + ref : ''}.pdf`;
        doc.save(name);
        const a = $('#pdfLink'); a.href = doc.output('bloburl'); a.download = name; a.hidden = false;
      } catch (e) { toast('Error al generar el PDF'); console.error(e); }
    };
    if (window.jspdf && window.jspdf.jsPDF && window.jspdf.jsPDF.API.autoTable) return go();
    // carga perezosa de las librerías del PDF
    const load = (src) => new Promise((ok, ko) => { const s = document.createElement('script'); s.src = src; s.onload = ok; s.onerror = ko; document.head.appendChild(s); });
    toast('Preparando el PDF…');
    load('jspdf.js').then(() => load('autotable.js')).then(go).catch(() => toast('No se pudo cargar el generador de PDF'));
  };

  startPolling(2000);
})();
