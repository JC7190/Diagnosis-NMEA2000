#!/usr/bin/env python3
"""Empaqueta la web (HTML+CSS+JS en un solo archivo), la comprime y genera web_assets.h para el firmware."""
import gzip, os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
web = os.path.join(root, 'web')
out_h = os.path.join(root, 'firmware', 'n2kdiag', 'web_assets.h')
dist = os.path.join(root, 'dist'); os.makedirs(dist, exist_ok=True)

def rd(p): return open(os.path.join(web, p), encoding='utf-8').read()

def slim(code):
    # minificación segura: quita sangrías, líneas vacías y comentarios de línea completa
    out = []
    for ln in code.split('\n'):
        t = ln.strip()
        if not t or t.startswith('//'):
            continue
        out.append(t)
    return '\n'.join(out)

logo = rd('brand/logo_inline.svg').strip()   # logo PoweryMax (blanco, color = currentColor)
html = rd('index.html').replace('<!--LOGO-->', logo)
html = html.replace('<link rel="stylesheet" href="style.css">', '<style>' + slim(rd('style.css')) + '</style>')
def inline(m): return '<script>' + slim(rd(m.group(1))).replace('</script', '<\\/script') + '</script>'
html = re.sub(r'<script src="((?:lookups|analyze|report|app)\.js)"></script>', inline, html)
open(os.path.join(dist, 'index.html'), 'w', encoding='utf-8').write(html)
open(os.path.join(dist, 'update.html'), 'w', encoding='utf-8').write(rd('update.html').replace('<!--LOGO-->', logo))

assets = [
    ('INDEX_HTML_GZ', html.encode('utf-8')),
    ('UPDATE_HTML_GZ', rd('update.html').replace('<!--LOGO-->', logo).encode('utf-8')),
    ('BRAND_JS_GZ', open(os.path.join(web, 'brand.js'), 'rb').read()),
    ('JSPDF_JS_GZ', open(os.path.join(web, 'lib', 'jspdf.umd.min.js'), 'rb').read()),
    ('AUTOTABLE_JS_GZ', open(os.path.join(web, 'lib', 'jspdf.plugin.autotable.min.js'), 'rb').read()),
]
with open(out_h, 'w') as f:
    f.write('// Generado por tools/build_web.py — no editar a mano\n#pragma once\n#include <Arduino.h>\n')
    for name, data in assets:
        gz = gzip.compress(data, 9, mtime=0)
        f.write(f'// {len(data)} bytes -> {len(gz)} gzip\nconst uint8_t {name}[] PROGMEM = {{\n')
        for i in range(0, len(gz), 24):
            f.write('  ' + ','.join(str(b) for b in gz[i:i + 24]) + ',\n')
        f.write('};\n')
        print(f'{name}: {len(data)} -> {len(gz)} bytes')
