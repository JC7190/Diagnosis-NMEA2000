// Recorre la interfaz como un iPhone contra el servidor de prueba (mock_server.py en :8765).
// Uso: node ui_test.js [carpeta_salida]
const { chromium, devices } = require('playwright');
const path = require('path');
const OUT = process.argv[2] || process.env.OUT || '/tmp';
const shot = (pg, name, full) => pg.screenshot({ path: path.join(OUT, name), fullPage: !!full });

(async () => {
  const b = await chromium.launch();
  const ctx = await b.newContext({ ...devices['iPhone 13'], acceptDownloads: true, locale: 'es-ES' });
  const pg = await ctx.newPage();
  const errs = [];
  pg.on('pageerror', (e) => errs.push(String(e)));
  pg.on('console', (m) => { if (m.type() === 'error') errs.push(m.text()); });
  const reqs = [];
  pg.on('request', (r) => reqs.push(r.url()));

  await pg.goto('http://127.0.0.1:8765/');
  await pg.waitForTimeout(2500);
  await shot(pg, 'ui1_home.png');
  await pg.click('#bSetup'); await pg.fill('#ref', 'Barco Marea · ORMA 123456');
  await pg.click('#segDur button[data-v="120"]');
  await shot(pg, 'ui2_setup.png');
  await pg.click('#bStart'); await pg.waitForTimeout(3200);
  await shot(pg, 'ui3_running.png');
  await pg.waitForSelector('#term.active', { timeout: 15000 });
  await shot(pg, 'ui4_term.png');
  await pg.click('#ansTerm button[data-v="120"]');
  await pg.waitForSelector('#water.active', { timeout: 5000 }); console.log('pregunta del transductor: OK');
  await shot(pg, 'ui4b_water.png');
  await pg.click('#ansWater button[data-v="no"]');
  await pg.waitForSelector('#result.active');
  await pg.waitForTimeout(400);
  await shot(pg, 'ui5_result.png');
  await shot(pg, 'ui5_result_full.png', true);
  const [dl] = await Promise.all([pg.waitForEvent('download'), pg.click('#bPdf')]);
  await dl.saveAs(path.join(OUT, 'ui_report.pdf')); console.log('descarga', dl.suggestedFilename());
  const full = reqs.filter((u) => /\/api\/snapshot$/.test(u)).length, lite = reqs.filter((u) => /lite=1/.test(u)).length;
  console.log(`sondeos ligeros ${lite}, completos ${full}`);
  const [csv] = await Promise.all([pg.waitForEvent('download'), pg.click('#bCsv')]);
  await csv.saveAs(path.join(OUT, 'captura.csv'));
  const txt = require('fs').readFileSync(path.join(OUT, 'captura.csv'), 'utf8').split('\n');
  console.log('captura CSV:', csv.suggestedFilename(), (txt.length - 4) + ' tramas;', txt[2]);

  // búsqueda guiada por tramos (la red del simulador tiene errores)
  await pg.click('#bSplit'); await pg.waitForSelector('#split.active');
  await shot(pg, 'ui6_split.png');
  await pg.click('#bSplitGo');
  await pg.waitForSelector('#spRes .spOut', { timeout: 30000 });
  console.log('tramo medido:', await pg.textContent('#spRes .spOut b'));
  await shot(pg, 'ui7_split_res.png', true);
  await pg.click('#bSplitEnd'); await pg.waitForSelector('#result.active');
  const [pdf2] = await Promise.all([pg.waitForEvent('download'), pg.click('#bPdf')]);
  await pdf2.saveAs(path.join(OUT, 'ui_report_split.pdf'));

  // recarga a mitad de prueba: debe volver a la pantalla correcta
  await pg.click('#bNew'); await pg.waitForSelector('#home.active');
  await pg.click('#bSetup'); await pg.click('#bStart'); await pg.waitForSelector('#running.active');
  await pg.reload(); await pg.waitForSelector('#running.active', { timeout: 5000 }); console.log('recarga en curso: OK');
  await pg.waitForSelector('#term.active', { timeout: 15000 }); console.log('fin tras recarga: OK');

  // cancelar a mitad de prueba
  await pg.request.get('http://127.0.0.1:8765/api/resume');
  await pg.goto('http://127.0.0.1:8765/'); await pg.waitForTimeout(1500);
  console.log('estado en inicio:', await pg.textContent('#stT'));
  await pg.click('#bSetup'); await pg.click('#bStart'); await pg.waitForSelector('#running.active');
  await shot(pg, 'ui3b_running.png');
  pg.once('dialog', (d) => d.accept());
  await pg.click('#bCancel'); await pg.waitForSelector('#home.active', { timeout: 5000 }); console.log('cancelar: OK');

  console.log('errores', errs);
  await b.close();
  process.exit(errs.length ? 1 : 0);
})();
