// Genera el informe PDF a partir del resultado de analyze()
(function (root) {
  const C = {
    navy: [20, 20, 22], teal: [225, 37, 45], gray: [160, 160, 168], text: [33, 37, 41], muted: [108, 117, 125], line: [222, 226, 230], bg: [246, 248, 250],
    fail: [198, 40, 40], warn: [214, 128, 0], ok: [46, 125, 50], info: [69, 90, 100], na: [130, 130, 138],
  };
  const SEV = { fail: 'AVERÍA', warn: 'REVISAR', ok: 'OK', info: 'INFO', na: 'NO MEDIDO' };
  const VERDICT = {
    fail: ['AVERÍA DETECTADA', 'Hay fallos que impiden que la red funcione bien. Sigue los pasos de "Qué hay que hacer".'],
    warn: ['REVISAR', 'La red funciona, pero hay puntos que conviene corregir.'],
    ok: ['RED CORRECTA', 'No se ha encontrado ningún problema en la red.'],
  };
  const TERM = { '60': ['Correctos', '60 ohm'], '120': ['Falta uno', '120 ohm'], '40': ['Sobran', '40 ohm o menos'], open: ['Abierto', 'resistencia muy alta'], short: ['Cortocircuito', 'menos de 10 ohm'], jump: ['Inestable', 'la lectura salta'], skip: ['No medido', ''] };

  // La fuente estándar del PDF solo tiene los caracteres de Windows-1252: lo demás
  // (flechas, símbolos, textos raros que mandan algunos equipos) se sustituye.
  const CP1252 = '€‚ƒ„…†‡ˆ‰Š‹ŒŽ‘’“”•–—˜™š›œžŸ';
  const clean = (s) => String(s == null ? '' : s)
    .replace(/→/g, '->').replace(/Ω/g, ' ohm').replace(/[✓✔]/g, '')
    .replace(/[^\u0000-ÿ]/g, (c) => (CP1252.includes(c) ? c : '?'))
    .replace(/[\u0000-\u001F\u007F]/g, ' ');

  function build(r, jsPDF) {
    const fmt = (x, d) => (x === null || x === undefined || isNaN(x)) ? '-' : Number(x).toFixed(d).replace('.', ',');
    const doc = new jsPDF({ unit: 'mm', format: 'a4', compress: true });
    const W = 210, M = 14, CW = W - 2 * M, BOTTOM = 280;
    const date = r.date instanceof Date ? r.date : new Date(r.date);
    const dstr = date.toLocaleDateString('es-ES', { day: '2-digit', month: '2-digit', year: 'numeric' }) + ' ' +
      date.toLocaleTimeString('es-ES', { hour: '2-digit', minute: '2-digit' });
    const ref = clean(r.ref);
    doc.setProperties({ title: 'PoweryMax Nmea2000 Diagnostic - Informe' + (ref ? ' - ' + ref : ''), creator: 'PoweryMax Nmea2000 Diagnostic' });

    // ---- Cabecera (marca PoweryMax: negro con franja roja) ----
    doc.setFillColor(...C.navy); doc.rect(0, 0, W, 30, 'F');
    doc.setFillColor(...C.teal); doc.rect(0, 30, W, 1.2, 'F');
    const B = root.BRAND || (typeof require !== 'undefined' ? (() => { try { return require('./brand.js'); } catch (e) { return null; } })() : null);
    if (B && B.logoPng) doc.addImage(B.logoPng, 'PNG', M, 6.5, 52, 52 * B.logoH / B.logoW);
    else { doc.setTextColor(255, 255, 255); doc.setFont('helvetica', 'bold'); doc.setFontSize(20); doc.text('PoweryMax', M, 15); }
    doc.setFont('helvetica', 'bold'); doc.setFontSize(8.5);
    doc.setTextColor(...C.gray); doc.text('NMEA2000', M, 22.5, { charSpace: 0.9 });
    const tw0 = doc.getTextWidth('NMEA2000') + 8 * 0.9 + 2.2;
    doc.setTextColor(...C.teal); doc.text('DIAGNOSTIC', M + tw0, 22.5, { charSpace: 0.9 });
    doc.setTextColor(255, 255, 255); doc.setFontSize(14); doc.text('Informe de diagnóstico', W - M, 12, { align: 'right' });
    doc.setFont('helvetica', 'normal'); doc.setFontSize(9); doc.setTextColor(205, 205, 210);
    doc.text(`${dstr}  ·  ${Math.round(r.metrics.dur / 1000)} s  ·  Prueba ${r.metrics.mode === 'active' ? 'completa' : 'solo escucha'}`, W - M, 18, { align: 'right' });
    if (ref) {
      doc.setFont('helvetica', 'bold'); doc.setTextColor(255, 255, 255);
      let t = ref; while (t.length > 3 && doc.getTextWidth(t) > 105) t = t.slice(0, -2);
      if (t !== ref) t = t.replace(/\s+$/, '') + '…';
      doc.text(t, W - M, 23.5, { align: 'right' });
    }

    // ---- Veredicto ----
    let y = 38;
    const vc = C[r.verdict];
    doc.setFontSize(10); doc.setFont('helvetica', 'normal');
    const vLines = doc.splitTextToSize(VERDICT[r.verdict][1], CW - 12);
    const vh = 15 + vLines.length * 4.6;
    doc.setFillColor(...vc); doc.roundedRect(M, y, CW, vh, 2.5, 2.5, 'F');
    doc.setTextColor(255, 255, 255); doc.setFont('helvetica', 'bold'); doc.setFontSize(17);
    doc.text(VERDICT[r.verdict][0], M + 6, y + 9.5);
    doc.setFontSize(10);
    const cnt = `${r.nFail} avería${r.nFail === 1 ? '' : 's'}  ·  ${r.nWarn} aviso${r.nWarn === 1 ? '' : 's'}`;
    doc.text(cnt, W - M - 6, y + 9.5, { align: 'right' });
    doc.setFont('helvetica', 'normal'); doc.setFontSize(10);
    doc.text(vLines, M + 6, y + 16);
    y += vh + 7;

    // ---- Indicadores ----
    const m = r.metrics, v = m.v || {};
    const tiles = [
      ['Equipos', String(m.nDev), ''],
      ['Tensión de red', v.ok ? `${fmt(v.min, 1)} - ${fmt(v.max, 1)} V` : 'No medida', v.ok ? `media ${fmt(v.avg, 1)} V` : ''],
      ['Carga de red', `${fmt(m.loadAvg, 0)} %`, `pico ${fmt(m.loadPeak, 0)} %`],
      ['Errores CAN', `${fmt(m.errRate, 2)} %`, `${m.busErr} en ${m.frames || 0} tramas`],
      ['Terminadores', (TERM[m.term] || ['-'])[0], (TERM[m.term] || ['', ''])[1]],
    ];
    const tw = (CW - 4 * 3) / 5;
    tiles.forEach((t, i) => {
      const x = M + i * (tw + 3);
      doc.setFillColor(...C.bg); doc.setDrawColor(...C.line); doc.roundedRect(x, y, tw, 20, 2, 2, 'FD');
      doc.setTextColor(...C.muted); doc.setFont('helvetica', 'normal'); doc.setFontSize(7.5); doc.text(t[0].toUpperCase(), x + 3, y + 5.5);
      doc.setTextColor(...C.text); doc.setFont('helvetica', 'bold'); doc.setFontSize(11.5);
      if (doc.getTextWidth(t[1]) > tw - 5) doc.setFontSize(9);
      doc.text(doc.splitTextToSize(t[1], tw - 5)[0], x + 3, y + 12);
      doc.setFont('helvetica', 'normal'); doc.setFontSize(7.5); doc.setTextColor(...C.muted);
      doc.text(doc.splitTextToSize(t[2], tw - 5)[0] || '', x + 3, y + 17);
    });
    y += 28;

    // ---- Qué hay que hacer ----
    const section = (title, need) => {
      if (y + (need || 20) > BOTTOM) { doc.addPage(); y = 18; }
      doc.setTextColor(...C.navy); doc.setFont('helvetica', 'bold'); doc.setFontSize(13); doc.text(title, M, y);
      doc.setDrawColor(...C.teal); doc.setLineWidth(0.6); doc.line(M, y + 2, M + 22, y + 2); doc.setLineWidth(0.2);
      y += 8;
    };
    section('Qué hay que hacer');
    if (!r.actions.length) {
      doc.setTextColor(...C.text); doc.setFont('helvetica', 'normal'); doc.setFontSize(10);
      doc.text('Nada que corregir. La red está en buen estado.', M, y); y += 8;
    }
    r.actions.forEach((a, i) => {
      doc.setFontSize(10);
      const titleLines = doc.setFont('helvetica', 'bold').splitTextToSize(clean(a.title), CW - 12);
      const bodyLines = doc.setFont('helvetica', 'normal').splitTextToSize(clean(a.text), CW - 12);
      const h = titleLines.length * 4.6 + bodyLines.length * 4.4 + 3;
      if (y + h > BOTTOM) { doc.addPage(); y = 18; }
      doc.setFillColor(...C[a.sev]); doc.circle(M + 3.2, y - 1.2, 3.2, 'F');
      doc.setTextColor(255, 255, 255); doc.setFont('helvetica', 'bold'); doc.setFontSize(8.5); doc.text(String(i + 1), M + 3.2, y + 0.1, { align: 'center' });
      doc.setTextColor(...C.text); doc.setFontSize(10); doc.text(titleLines, M + 10, y);
      y += titleLines.length * 4.6;
      doc.setFont('helvetica', 'normal'); doc.setTextColor(...C.muted); doc.text(bodyLines, M + 10, y);
      y += bodyLines.length * 4.4 + 4;
    });

    const tableBase = {
      margin: { left: M, right: M, top: 16, bottom: 297 - BOTTOM },
      styles: { font: 'helvetica', fontSize: 8.5, cellPadding: 2, textColor: C.text, lineColor: C.line, lineWidth: 0.1, overflow: 'linebreak', valign: 'middle' },
      headStyles: { fillColor: C.navy, textColor: 255, fontStyle: 'bold', fontSize: 8.5 },
      alternateRowStyles: { fillColor: [250, 251, 252] },
      rowPageBreak: 'avoid',
    };
    const sevCell = (data, col) => {
      if (data.section === 'body' && data.column.index === col) {
        const k = data.cell.raw && data.cell.raw.sev;
        if (k) { data.cell.styles.textColor = C[k]; data.cell.styles.fontStyle = 'bold'; data.cell.text = [SEV[k]]; }
      }
    };

    // ---- Detalle ----
    y += 2; section('Detalle de las comprobaciones', 40);
    doc.autoTable(Object.assign({}, tableBase, {
      startY: y,
      head: [['Estado', 'Área', 'Resultado', 'Detalle']],
      body: r.findings.map((f) => [{ content: '', sev: f.sev }, clean(f.area), clean(f.title), clean(f.detail)]),
      columnStyles: { 0: { cellWidth: 19 }, 1: { cellWidth: 26 }, 2: { cellWidth: 55, fontStyle: 'bold' } },
      didParseCell: (d) => sevCell(d, 0),
    }));
    y = doc.lastAutoTable.finalY + 10;

    // ---- Equipos ----
    section('Equipos encontrados en la red', 40);
    if (!r.devices.length) {
      doc.setTextColor(...C.muted); doc.setFont('helvetica', 'normal'); doc.setFontSize(10);
      doc.text('No se ha detectado ningún equipo.', M, y); y += 8;
    } else {
      doc.autoTable(Object.assign({}, tableBase, {
        startY: y,
        head: [['Dir.', 'Equipo', 'Fabricante', 'Tipo', 'Software', 'Nº de serie', 'Estado']],
        body: r.devices.map((d) => [String(d.sa), clean(d.pi && d.pi.model ? d.pi.model : d.label), clean(d.mfrName), clean(d.typeName),
          clean(d.pi && d.pi.sw ? d.pi.sw : '-'), clean(d.pi && d.pi.serial ? d.pi.serial : '-'), { content: '', sev: d.sev }]),
        columnStyles: { 0: { cellWidth: 10, halign: 'center' }, 1: { fontStyle: 'bold' }, 6: { cellWidth: 19 } },
        didParseCell: (d) => sevCell(d, 6),
      }));
      y = doc.lastAutoTable.finalY + 4;
      if (y + 10 > BOTTOM) { doc.addPage(); y = 18; }
      doc.setFont('helvetica', 'italic'); doc.setFontSize(8); doc.setTextColor(...C.muted);
      doc.text(doc.splitTextToSize('Compara las versiones de software con las últimas publicadas en la web de cada fabricante. Un software antiguo es causa frecuente de fallos de comunicación.', CW), M, y + 2);
      y += 12;
    }

    // ---- Anexo: mensajes ----
    if (r.pgnRows.length) {
      y += 2; section('Anexo: mensajes de cada equipo', 50);
      doc.autoTable(Object.assign({}, tableBase, {
        startY: y,
        head: [['Dir.', 'Equipo', 'PGN', 'Mensaje', 'Recibidos', 'Cada', 'Normal']],
        body: r.pgnRows.map((p) => [String(p.sa), clean(p.dev), String(p.pgn), clean(p.name), String(p.n), p.every, p.expected]),
        styles: Object.assign({}, tableBase.styles, { fontSize: 7.5, cellPadding: 1.5 }),
        columnStyles: { 0: { cellWidth: 10, halign: 'center' }, 2: { cellWidth: 15 }, 4: { cellWidth: 17, halign: 'right' }, 5: { cellWidth: 14, halign: 'right' }, 6: { cellWidth: 14, halign: 'right' } },
      }));
    }

    // ---- Pie ----
    const n = doc.getNumberOfPages();
    const foot = clean('PoweryMax Nmea2000 Diagnostic' + (m.fw ? ' v' + m.fw : '') + ' · ' + dstr + (ref ? ' · ' + ref : ''));
    for (let i = 1; i <= n; i++) {
      doc.setPage(i);
      doc.setDrawColor(...C.line); doc.setLineWidth(0.2); doc.line(M, 286, W - M, 286);
      doc.setFont('helvetica', 'normal'); doc.setFontSize(7.5); doc.setTextColor(...C.muted);
      doc.text(doc.splitTextToSize(foot, CW - 30)[0], M, 290.5);
      doc.text(`Página ${i} de ${n}`, W - M, 290.5, { align: 'right' });
    }
    return doc;
  }

  const API = { build };
  if (typeof module !== 'undefined') module.exports = API; else root.N2KReport = API;
})(typeof window !== 'undefined' ? window : globalThis);
