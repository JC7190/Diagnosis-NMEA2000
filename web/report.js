// Genera el informe PDF a partir del resultado de analyze()
(function (root) {
  const C = {
    navy: [15, 42, 68], teal: [0, 128, 128], text: [33, 37, 41], muted: [108, 117, 125], line: [222, 226, 230], bg: [246, 248, 250],
    fail: [198, 40, 40], warn: [224, 138, 0], ok: [46, 125, 50], info: [69, 90, 100],
  };
  const SEV = { fail: 'AVERÍA', warn: 'REVISAR', ok: 'OK', info: 'INFO' };
  const VERDICT = {
    fail: ['AVERÍA DETECTADA', 'Hay fallos que impiden que la red funcione bien. Sigue los pasos de "Qué hay que hacer".'],
    warn: ['REVISAR', 'La red funciona, pero hay puntos que conviene corregir.'],
    ok: ['RED CORRECTA', 'No se ha encontrado ningún problema en la red.'],
  };
  const TERM = { '60': ['Correctos', '60 ohm'], '120': ['Falta uno', '120 ohm'], '40': ['Sobran', '40 ohm o menos'], open: ['Abierto', 'resistencia muy alta'], short: ['Cortocircuito', 'casi 0 ohm'], skip: ['No medido', ''] };

  function build(r, jsPDF) {
    const fmt = (x, d) => (x === null || x === undefined || isNaN(x)) ? '-' : Number(x).toFixed(d).replace('.', ',');
    const doc = new jsPDF({ unit: 'mm', format: 'a4' });
    const W = 210, M = 14, CW = W - 2 * M;
    const date = r.date instanceof Date ? r.date : new Date(r.date);
    const dstr = date.toLocaleDateString('es-ES', { day: '2-digit', month: '2-digit', year: 'numeric' }) + ' ' +
      date.toLocaleTimeString('es-ES', { hour: '2-digit', minute: '2-digit' });

    // ---- Cabecera ----
    doc.setFillColor(...C.navy); doc.rect(0, 0, W, 30, 'F');
    doc.setTextColor(255, 255, 255); doc.setFont('helvetica', 'bold'); doc.setFontSize(18);
    doc.text('Informe de diagnóstico NMEA 2000', M, 14);
    doc.setFont('helvetica', 'normal'); doc.setFontSize(10);
    doc.text(`${dstr}   ·   Duración ${Math.round(r.metrics.dur / 1000)} s   ·   Modo ${r.metrics.mode === 'active' ? 'completo (activo)' : 'solo escucha'}`, M, 22);
    if (r.ref) { doc.setFont('helvetica', 'bold'); doc.text(r.ref, W - M, 22, { align: 'right' }); }

    // ---- Veredicto ----
    let y = 38;
    const vc = C[r.verdict];
    doc.setFillColor(...vc); doc.roundedRect(M, y, CW, 22, 2.5, 2.5, 'F');
    doc.setTextColor(255, 255, 255); doc.setFont('helvetica', 'bold'); doc.setFontSize(17);
    doc.text(VERDICT[r.verdict][0], M + 6, y + 9.5);
    doc.setFontSize(10);
    const cnt = `${r.nFail} avería${r.nFail === 1 ? '' : 's'}  ·  ${r.nWarn} aviso${r.nWarn === 1 ? '' : 's'}`;
    doc.text(cnt, W - M - 6, y + 9.5, { align: 'right' });
    doc.setFont('helvetica', 'normal'); doc.setFontSize(10);
    doc.text(VERDICT[r.verdict][1], M + 6, y + 17);
    y += 29;

    // ---- Indicadores ----
    const m = r.metrics, v = m.v || {};
    const tiles = [
      ['Equipos', String(m.nDev), ''],
      ['Tensión de red', v.ok ? `${fmt(v.min, 1)}-${fmt(v.max, 1)} V` : 'No medida', v.ok ? `media ${fmt(v.avg, 1)} V` : ''],
      ['Carga de red', `${fmt(m.loadAvg, 0)} %`, `pico ${fmt(m.loadPeak, 0)} %`],
      ['Errores CAN', `${fmt(m.errRate, 2)} %`, `${m.busErr} en ${m.frames || 0} tramas`],
      ['Terminadores', (TERM[m.term] || ['-'])[0], (TERM[m.term] || ['', ''])[1]],
    ];
    const tw = (CW - 4 * 3) / 5;
    tiles.forEach((t, i) => {
      const x = M + i * (tw + 3);
      doc.setFillColor(...C.bg); doc.setDrawColor(...C.line); doc.roundedRect(x, y, tw, 20, 2, 2, 'FD');
      doc.setTextColor(...C.muted); doc.setFont('helvetica', 'normal'); doc.setFontSize(7.5); doc.text(t[0].toUpperCase(), x + 3, y + 5.5);
      doc.setTextColor(...C.text); doc.setFont('helvetica', 'bold'); doc.setFontSize(t[1].length > 12 ? 9 : 11.5);
      doc.text(doc.splitTextToSize(t[1], tw - 5)[0], x + 3, y + 12);
      doc.setFont('helvetica', 'normal'); doc.setFontSize(7.5); doc.setTextColor(...C.muted); doc.text(t[2], x + 3, y + 17);
    });
    y += 28;

    // ---- Qué hay que hacer ----
    const section = (title) => {
      if (y > 265) { doc.addPage(); y = 18; }
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
      const titleLines = doc.setFont('helvetica', 'bold').splitTextToSize(a.title, CW - 12);
      const bodyLines = doc.setFont('helvetica', 'normal').splitTextToSize(a.text, CW - 12);
      const h = titleLines.length * 4.6 + bodyLines.length * 4.4 + 3;
      if (y + h > 282) { doc.addPage(); y = 18; }
      doc.setFillColor(...C[a.sev]); doc.circle(M + 3.2, y - 1.2, 3.2, 'F');
      doc.setTextColor(255, 255, 255); doc.setFont('helvetica', 'bold'); doc.setFontSize(8.5); doc.text(String(i + 1), M + 3.2, y + 0.1, { align: 'center' });
      doc.setTextColor(...C.text); doc.setFontSize(10); doc.text(titleLines, M + 10, y);
      y += titleLines.length * 4.6;
      doc.setFont('helvetica', 'normal'); doc.setTextColor(...C.muted); doc.text(bodyLines, M + 10, y);
      y += bodyLines.length * 4.4 + 4;
    });

    const tableBase = {
      margin: { left: M, right: M, top: 16 },
      styles: { font: 'helvetica', fontSize: 8.5, cellPadding: 2, textColor: C.text, lineColor: C.line, lineWidth: 0.1, overflow: 'linebreak' },
      headStyles: { fillColor: C.navy, textColor: 255, fontStyle: 'bold', fontSize: 8.5 },
      alternateRowStyles: { fillColor: [250, 251, 252] },
    };
    const sevCell = (data, col) => {
      if (data.section === 'body' && data.column.index === col) {
        const k = data.cell.raw && data.cell.raw.sev;
        if (k) { data.cell.styles.textColor = C[k]; data.cell.styles.fontStyle = 'bold'; data.cell.text = [SEV[k]]; }
      }
    };

    // ---- Detalle ----
    y += 2; section('Detalle de las comprobaciones');
    doc.autoTable(Object.assign({}, tableBase, {
      startY: y,
      head: [['Estado', 'Área', 'Resultado', 'Detalle']],
      body: r.findings.map((f) => [{ content: '', sev: f.sev }, f.area, f.title, f.detail || '']),
      columnStyles: { 0: { cellWidth: 18 }, 1: { cellWidth: 26 }, 2: { cellWidth: 55, fontStyle: 'bold' } },
      didParseCell: (d) => sevCell(d, 0),
    }));
    y = doc.lastAutoTable.finalY + 10;

    // ---- Equipos ----
    section('Equipos encontrados en la red');
    doc.autoTable(Object.assign({}, tableBase, {
      startY: y,
      head: [['Dir.', 'Equipo', 'Fabricante', 'Tipo', 'Software', 'Nº de serie', 'Estado']],
      body: r.devices.map((d) => [String(d.sa), d.pi ? d.pi.model : d.label, d.mfrName, d.typeName,
        d.pi ? d.pi.sw || '-' : '-', d.pi ? d.pi.serial || '-' : '-', { content: '', sev: d.sev }]),
      columnStyles: { 0: { cellWidth: 10, halign: 'center' }, 1: { fontStyle: 'bold' }, 6: { cellWidth: 18 } },
      didParseCell: (d) => sevCell(d, 6),
    }));
    y = doc.lastAutoTable.finalY + 4;
    doc.setFont('helvetica', 'italic'); doc.setFontSize(8); doc.setTextColor(...C.muted);
    doc.text(doc.splitTextToSize('Compara las versiones de software con las últimas publicadas en la web de cada fabricante. Un software antiguo es causa frecuente de fallos de comunicación.', CW), M, y + 2);
    y += 12;

    // ---- Anexo: mensajes ----
    y += 2; if (y > 230) { doc.addPage(); y = 18; }
    section('Anexo: mensajes de cada equipo');
    doc.autoTable(Object.assign({}, tableBase, {
      startY: y,
      head: [['Dir.', 'Equipo', 'PGN', 'Mensaje', 'Recibidos', 'Cada', 'Normal']],
      body: r.pgnRows.map((p) => [String(p.sa), p.dev, String(p.pgn), p.name, String(p.n), p.every, p.expected]),
      styles: Object.assign({}, tableBase.styles, { fontSize: 7.5, cellPadding: 1.5 }),
      columnStyles: { 0: { cellWidth: 10, halign: 'center' }, 2: { cellWidth: 15 }, 4: { cellWidth: 17, halign: 'right' }, 5: { cellWidth: 14, halign: 'right' }, 6: { cellWidth: 14, halign: 'right' } },
    }));

    // ---- Pie ----
    const n = doc.getNumberOfPages();
    for (let i = 1; i <= n; i++) {
      doc.setPage(i);
      doc.setDrawColor(...C.line); doc.line(M, 287, W - M, 287);
      doc.setFont('helvetica', 'normal'); doc.setFontSize(7.5); doc.setTextColor(...C.muted);
      doc.text('Diagnóstico NMEA 2000 · ' + dstr + (r.ref ? ' · ' + r.ref : ''), M, 291.5);
      doc.text(`Página ${i} de ${n}`, W - M, 291.5, { align: 'right' });
    }
    return doc;
  }

  const API = { build };
  if (typeof module !== 'undefined') module.exports = API; else root.N2KReport = API;
})(typeof window !== 'undefined' ? window : globalThis);
