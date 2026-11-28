const { chromium } = require('playwright'); const fs = require('fs');
const S = process.argv[2];
const logo = fs.readFileSync('/home/user/Diagnosis-NMEA2000/web/brand/logo_inline.svg', 'utf8').replace('class="plogo"', 'style="width:470px;height:auto;display:block;color:#fff"');
const icon = `<div style="width:512px;height:512px;background:radial-gradient(120% 80% at 50% 0%,#2a2a2e 0%,#0a0a0b 70%);display:flex;flex-direction:column;align-items:center;justify-content:center;font-family:-apple-system,Helvetica,Arial,sans-serif">
  ${logo}
  <div style="width:470px;height:9px;background:#e1252d;border-radius:4px;margin:22px 0 26px"></div>
  <div style="color:#d4d4d8;font-weight:800;font-size:76px;letter-spacing:3px;line-height:1">NMEA2000</div>
  <div style="color:#e1252d;font-weight:800;font-size:66px;letter-spacing:4px;line-height:1;margin-top:14px">DIAGNOSTIC</div></div>`;
(async () => {
  const b = await chromium.launch(); const p = await b.newPage({ viewport: { width: 512, height: 512 } });
  await p.setContent(`<body style="margin:0">${icon}</body>`); await p.screenshot({ path: `${S}/icon/icon_c.png` });
  const img = 'data:image/png;base64,' + fs.readFileSync(`${S}/icon/icon_c.png`).toString('base64');
  const p2 = await b.newPage({ viewport: { width: 420, height: 440 } });
  await p2.setContent(`<body style="margin:0;background:linear-gradient(160deg,#5b6a80,#2c3442);display:flex;flex-direction:column;align-items:center;gap:10px;padding:30px 0;font-family:-apple-system,Helvetica,Arial">
    <img src="${img}" style="width:200px;height:200px;border-radius:45px;box-shadow:0 6px 20px #0006">
    <div style="color:#fff;font:600 26px -apple-system,Helvetica,Arial">PoweryMax N2K</div>
    <img src="${img}" style="width:60px;height:60px;border-radius:14px;margin-top:14px">
    <div style="color:#fff;font:600 13px -apple-system,Helvetica,Arial">PoweryMax N2K</div>
    <div style="color:#bbb;font:15px -apple-system,Helvetica,Arial">tamaño real</div></body>`);
  await p2.screenshot({ path: `${S}/icon/icono_PoweryMax_v2.png` }); await b.close();
})();
