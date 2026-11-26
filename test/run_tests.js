// Pruebas automáticas: simulador (C++) → snapshot → analyze.js → comprobaciones.
// Uso: node run_tests.js   (compila ./sim si hace falta)
const { execFileSync } = require('child_process');
const path = require('path');
const fs = require('fs');
const A = require('../web/analyze.js');

const here = __dirname;
const simBin = path.join(here, 'sim');
execFileSync('g++', ['-std=c++17', '-O2', '-Wall', '-I../firmware/n2kdiag', 'sim.cpp', '../firmware/n2kdiag/n2k_core.cpp', '-o', simBin], { cwd: here, stdio: 'inherit' });

let fails = 0;
const ok = (cond, msg) => { console.log(`  ${cond ? '✓' : '✗'} ${msg}`); if (!cond) fails++; };
const run = (sc, lite) => {
  const out = execFileSync(simBin, [sc].concat(lite ? ['lite'] : []), { maxBuffer: 64 << 20, stdio: ['ignore', 'pipe', 'inherit'] }).toString();
  return JSON.parse(out);   // falla si el JSON no es válido
};
const has = (r, sev, re) => r.findings.some((f) => (!sev || f.sev === sev) && re.test(f.title));
const list = (r) => r.findings.filter((f) => f.sev === 'fail' || f.sev === 'warn').map((f) => `${f.sev}: ${f.title}`);

// ---------- 1. Red con averías: todo debe detectarse ----------
console.log('\n[averias]');
{
  const s = run('averias');
  fs.writeFileSync(path.join(here, 'snapshot.json'), JSON.stringify(s));
  const r = A.analyze(s, { term: '120' });
  console.log('   ' + list(r).join('\n   '));
  ok(r.verdict === 'fail', 'veredicto AVERÍA');
  ok(has(r, 'fail', /Falta un terminador/), 'terminador ausente');
  ok(has(r, 'fail', /no consigue dirección/), 'equipo en dirección 254');
  ok(has(r, 'fail', /Conflicto de dirección \(dir\. 40\)/), 'conflicto real de dirección (40)');
  ok(has(r, 'fail', /Instancia de motor duplicada/), 'instancia de motor duplicada');
  ok(has(r, 'fail', /Presión de aceite baja/) && !has(r, null, /Presión de aceite baja \(con el motor parado\)/), 'alarma de aceite con el motor en marcha = AVERÍA');
  ok(has(r, 'warn', /Ghost 52in se desconecta a ratos/), 'Ghost se desconecta y se reinicia');
  ok(r.findings.some((f) => /Ghost/.test(f.title) && /reinici/.test(f.detail)), 'reinicio del Ghost detectado');
  ok(has(r, 'warn', /Mensajes incompletos de Yamaha Gateway \(dir\. 11\)/), 'fast packets cortados del motor 2');
  ok(has(r, 'warn', /Batería 0 baja/), 'batería baja');
  ok(has(r, 'warn', /no da profundidad/), 'sonda sin profundidad');
  ok(has(r, 'warn', /Tensión inestable/), 'tensión inestable');
  ok(has(r, 'warn', /Errores de comunicación/), 'errores CAN');
  const ghost = s.devices.find((d) => d.sa === 3);
  ok(ghost.claimsUnreq === 1, `reinicios del Ghost contados una vez (${ghost.claimsUnreq})`);
  const y = s.devices.find((d) => d.sa === 11);
  ok(y.fpErr === 3, `un error por mensaje cortado (${y.fpErr} de 3)`);
}

// ---------- 2. Red sana con trampas: ningún falso positivo ----------
console.log('\n[limpia]');
{
  const s = run('limpia');
  const r = A.analyze(s, { term: '60' });
  console.log('   ' + (list(r).join('\n   ') || '(sin avisos)'));
  ok(r.verdict === 'ok', `veredicto RED CORRECTA (${r.verdict})`);
  ok(!has(r, null, /reinici/i), 'petición de direcciones de otra pantalla ≠ reinicio');
  ok(!has(r, 'fail', /Conflicto/), 'reparto normal de direcciones ≠ conflicto');
  ok(has(r, 'info', /Cambio de dirección resuelto/), 'reparto de direcciones informado');
  ok(has(r, 'info', /con el motor parado/), 'alarmas con motor parado solo informativas');
  ok(!has(r, null, /duplicada/), 'temperaturas de distinta fuente, timón y equipo que cambia de dirección ≠ duplicado');
  ok(!has(r, null, /Batería 1/), 'batería de 36 V del motor eléctrico ≠ tensión alta');
  ok(!has(r, null, /desconecta|desapareció/), 'AIS con ráfagas ≠ desconexión');
  ok(!has(r, null, /señal GPS pobre/), 'HDOP/satélites "no disponible" ≠ señal pobre');
  ok(!has(r, null, /Consumo de red/), 'LEN "no disponible" no suma consumo');
  ok(!has(r, null, /incomplet/), 'trozos sueltos al empezar ≠ mensajes incompletos');
  ok(s.bus.fpErr === 0, `fpErr total 0 (${s.bus.fpErr})`);
}

// ---------- 3. Red grande y ruidosa ----------
console.log('\n[grande]');
{
  const s = run('grande');
  const r = A.analyze(s, { term: '60' });
  const fpErr = s.devices.reduce((a, d) => a + d.fpErr, 0), fpOk = s.devices.reduce((a, d) => a + (d.fpOk || 0), 0);
  ok(s.devices.length === 120, `120 equipos (${s.devices.length})`);
  ok(fpOk > 5000, `fast packets intercalados reensamblados (${fpOk} ok, ${fpErr} rotos)`);
  ok(fpErr < fpOk * 0.01, 'roturas proporcionales al ruido (< 1 %)');
  ok(s.bus.pgOverflow > 0 && s.pgns.length === 400, `tabla de PGN llena sin fallar (${s.pgns.length}, desbordes ${s.bus.pgOverflow})`);
  ok(s.bus.infoOverflow > 0, 'pool de información de producto lleno, avisado');
  ok(has(r, 'info', /Red muy grande/), 'aviso de red muy grande');
  ok(!has(r, null, /no envió su información/), 'sin falsos "no envió información" al llenarse el pool');
  const lite = run('grande', true);
  ok(!lite.pgns && lite.devices.length === 120, 'modo ligero sin tabla de PGN');
  const big = JSON.stringify(s).length, small = JSON.stringify(lite).length;
  console.log(`   JSON completo ${(big / 1024).toFixed(0)} KB, ligero ${(small / 1024).toFixed(0)} KB`);
}

console.log(fails ? `\n✗ ${fails} comprobaciones fallidas` : '\n✓ Todas las pruebas pasan');
process.exit(fails ? 1 : 0);
