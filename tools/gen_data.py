#!/usr/bin/env python3
"""Genera tablas a partir de canboat.json: lista de PGNs fast-packet (C) y lookups (JS)."""
import json, sys, os
src = sys.argv[1]; root = sys.argv[2]
d = json.load(open(src))
fast = sorted({p['PGN'] for p in d['PGNs'] if p.get('Type') == 'Fast'})
with open(os.path.join(root, 'firmware/n2kdiag/fast_pgns.h'), 'w') as f:
    f.write('// Generado desde canboat.json - PGNs que usan fast packet\n#pragma once\n#include <stdint.h>\n')
    f.write('static const uint32_t FAST_PGNS[] = {\n')
    for i in range(0, len(fast), 8):
        f.write('  ' + ', '.join(str(x) for x in fast[i:i+8]) + ',\n')
    f.write('};\nstatic const int FAST_PGNS_N = %d;\n' % len(fast))
le = {x['Name']: x for x in d['LookupEnumerations']}
mfr = {e['Value']: e['Name'] for e in le['MANUFACTURER_CODE']['EnumValues']}
cls = {e['Value']: e['Name'] for e in le['DEVICE_CLASS']['EnumValues']}
fn = {}
for e in [x for x in d['LookupIndirectEnumerations'] if x['Name'] == 'DEVICE_FUNCTION'][0]['EnumValues']:
    fn['%d_%d' % (e['Value1'], e['Value2'])] = e['Name']
pg = {}
for p in d['PGNs']:
    n = p['PGN']
    if n not in pg:
        pg[n] = {'d': p['Description'], 'i': p.get('TransmissionInterval'), 'f': 1 if p.get('Type') == 'Fast' else 0}
    else:
        # varias definiciones (propietarias): nombre genérico
        if 'Manufacturer' in p.get('Description', '') or p.get('Fallback'):
            pass
with open(os.path.join(root, 'web/lookups.js'), 'w') as f:
    f.write('// Generado desde canboat (Apache-2.0)\n')
    f.write('var LK = ' + json.dumps({'mfr': mfr, 'cls': cls, 'fn': fn, 'pgn': pg}, separators=(',', ':'), ensure_ascii=False) + ';\n')
    f.write("if (typeof module !== 'undefined') module.exports = LK;\n")
print('fast', len(fast), 'mfr', len(mfr), 'pgn', len(pg))
