# Servidor de prueba que imita al ESP32 usando snapshot.json
import json, time, http.server, socketserver, os, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
SNAP = json.load(open(os.path.join(ROOT, 'snapshot.json')))
STATE = {'start': None, 'dur': 6.0, 'mode': 'active'}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, body, ctype):
        self.send_response(200); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path.split('?')[0]
        if p == '/': return self.send(open(os.path.join(ROOT, '../dist/index.html'), 'rb').read(), 'text/html; charset=utf-8')
        if p == '/jspdf.js': return self.send(open(os.path.join(ROOT, '../web/lib/jspdf.umd.min.js'), 'rb').read(), 'application/javascript')
        if p == '/brand.js': return self.send(open(os.path.join(ROOT, '../web/brand.js'), 'rb').read(), 'application/javascript')
        if p == '/update': return self.send(open(os.path.join(ROOT, '../dist/update.html'), 'rb').read(), 'text/html; charset=utf-8')
        if p == '/autotable.js': return self.send(open(os.path.join(ROOT, '../web/lib/jspdf.plugin.autotable.min.js'), 'rb').read(), 'application/javascript')
        if p == '/api/start':
            STATE['start'] = time.time(); STATE['mode'] = 'passive' if 'passive' in self.path else 'active'
            return self.send(b'{"ok":1}', 'application/json')
        if p == '/api/frames':
            # tramas de prueba: ~400 por segundo mientras dura la prueba
            frm = int(self.path.split('from=')[1]) if 'from=' in self.path else 0
            el = min(time.time() - STATE['start'], STATE['dur']) if STATE['start'] else 0
            total = int(el * 400)
            lines = ['#seq=%d;lost=0' % total] + ['%d;09F80105;8;0102030405060708;' % (i * 2500) for i in range(frm, total)]
            return self.send(('\n'.join(lines) + '\n').encode(), 'text/plain')
        if p == '/api/resume':
            STATE['start'] = None; return self.send(b'{"ok":1}', 'application/json')
        if p == '/api/snapshot':
            s = json.loads(json.dumps(SNAP)); L = s['live']
            if STATE['start']:
                el = min(time.time() - STATE['start'], STATE['dur'])
                frac = el / STATE['dur']
                L['diag'] = {'running': int(frac < 1), 'elapsed': int(el * 1000), 'duration': int(STATE['dur'] * 1000), 'done': int(frac >= 1)}
                s['bus']['frames'] = int(s['bus']['frames'] * frac)
                L['twai']['busErr'] = int(L['twai']['busErr'] * frac)
                if frac < 1: s['devices'] = s['devices'][:max(1, int(len(s['devices']) * frac + 1))]
                if frac >= 1: s['bus']['frames'] = SNAP['bus']['frames']; L['twai']['busErr'] = SNAP['live']['twai']['busErr']
            else:
                L['diag'] = {'running': 0, 'elapsed': 0, 'duration': 0, 'done': 0}
            L['mode'] = STATE['mode']
            return self.send(json.dumps(s).encode(), 'application/json')
        self.send_response(404); self.end_headers()
socketserver.TCPServer.allow_reuse_address = True
with socketserver.ThreadingTCPServer(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H) as srv: srv.serve_forever()
