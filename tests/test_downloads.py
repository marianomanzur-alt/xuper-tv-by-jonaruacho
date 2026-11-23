#!/usr/bin/env python3
"""Pruebas de descarga offline contra un servidor HTTP local (sin red externa).
Uso: python tests/test_downloads.py build-host/download_probe build-host/manager_probe"""
import hashlib, http.server, json, os, re, shutil, subprocess, sys, tempfile, threading, time

PROBE, MANAGER = sys.argv[1], sys.argv[2]
DATA = bytes((i * 7 + i // 251 + 1) % 256 for i in range(6 * 1024 * 1024))
DATA_SHA = hashlib.sha256(DATA).hexdigest()
SRT = b"1\n00:00:00,000 --> 00:00:05,000\nHola\n"
LOG = []
CUT_SEEN = {"n": 0}

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def handle(self):
        try: super().handle()
        except (BrokenPipeError, ConnectionResetError): pass
    def _range(self):
        r = self.headers.get("Range")
        m = re.match(r"bytes=(\d+)-", r or "")
        return int(m.group(1)) if m else None
    def _send(self, code, body, ctype="video/mp4", extra=None, length=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body) if length is None else length))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
    def do_GET(self):
        path = self.path.split("?")[0]
        LOG.append((path, self.headers.get("Range"), self.headers.get("Content-Auth")))
        start = self._range()
        if path in ("/movie.mp4", "/movie.mkv"):
            if start is None:
                self._send(200, DATA); self.wfile.write(DATA)
            elif start >= len(DATA):
                self.send_response(416); self.send_header("Content-Range", "bytes */%d" % len(DATA)); self.send_header("Content-Length", "0"); self.end_headers()
            else:
                body = DATA[start:]
                self._send(206, body, extra={"Content-Range": "bytes %d-%d/%d" % (start, len(DATA) - 1, len(DATA))}); self.wfile.write(body)
        elif path == "/norange.mp4":
            self._send(200, DATA); self.wfile.write(DATA)
        elif path == "/slow.mp4":
            body = DATA[(start or 0):]
            hdr = {"Content-Range": "bytes %d-%d/%d" % (start, len(DATA) - 1, len(DATA))} if start else None
            self._send(206 if start else 200, body, extra=hdr)
            try:
                for i in range(0, len(body), 64 * 1024):
                    self.wfile.write(body[i:i + 64 * 1024]); self.wfile.flush(); time.sleep(0.05)
            except (BrokenPipeError, ConnectionResetError): pass
        elif path == "/cut.mp4":
            if start is None:
                CUT_SEEN["n"] += 1
                self._send(200, b"", length=1024 * 1024)
                self.wfile.write(DATA[:300 * 1024]); self.wfile.flush(); self.close_connection = True
            else:
                body = DATA[start:1024 * 1024]
                self._send(206, body, extra={"Content-Range": "bytes %d-%d/%d" % (start, 1024 * 1024 - 1, 1024 * 1024)}); self.wfile.write(body)
        elif path == "/list.m3u8":
            b = b"#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:10,\nseg1.ts\n"; self._send(200, b, "application/vnd.apple.mpegurl"); self.wfile.write(b)
        elif path == "/error.mp4":
            b = b"<html><body>Forbidden by CDN</body></html>"; self._send(200, b, "text/html"); self.wfile.write(b)
        elif path == "/gone.mp4":
            self._send(404, b"nope", "text/plain"); self.wfile.write(b"nope")
        elif path == "/auth.mp4":
            if self.headers.get("Content-Auth") != "ok":
                self._send(403, b"no", "text/plain"); self.wfile.write(b"no")
            else:
                self._send(200, DATA[:200000]); self.wfile.write(DATA[:200000])
        elif path == "/redir.mp4":
            self.send_response(302); self.send_header("Location", "/movie.mp4"); self.send_header("Content-Length", "0"); self.end_headers()
        elif path == "/sub1.srt" or path == "/sub2.srt":
            self._send(200, SRT, "text/plain"); self.wfile.write(SRT)
        else:
            self._send(404, b"", "text/plain")

class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

srv = Server(("127.0.0.1", 0), Handler)
BASE = "http://127.0.0.1:%d" % srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

passed = 0
def check(cond, label):
    global passed
    if not cond:
        print("FALLO:", label); sys.exit(1)
    passed += 1

def sha(path):
    with open(path, "rb") as f: return hashlib.sha256(f.read()).hexdigest()

def probe(path, tmp, name, *extra):
    part, final = os.path.join(tmp, name + ".part"), os.path.join(tmp, name + ".mp4")
    out = subprocess.run([PROBE, BASE + path, part, final, *extra], capture_output=True, text=True, timeout=60)
    check(out.returncode == 0, "probe ejecuta " + path + out.stderr)
    return json.loads(out.stdout.strip().splitlines()[-1]), part, final

OK, CANCELLED, PAUSED, HLS, INVALID, SPACE, WRITE, NETWORK, HTTP = range(9)
tmp = tempfile.mkdtemp(prefix="xs-dl-")
try:
    # 1) descarga completa, integridad y progreso
    r, part, final = probe("/movie.mp4", tmp, "full")
    check(r["status"] == OK and r["bytes"] == len(DATA), "descarga completa")
    check(sha(final) == DATA_SHA and not os.path.exists(part), "contenido identico y sin .part")
    check(r["progress_done"] == len(DATA) and r["progress_total"] == len(DATA), "progreso final 100%")

    # 2) cancelar a medias deja .part y reanudar completa sin repetir lo ya bajado
    LOG.clear()
    r, part, final = probe("/slow.mp4", tmp, "resume", "--cancel-at", str(1024 * 1024))
    check(r["status"] == CANCELLED and os.path.exists(part) and not os.path.exists(final), "cancelar conserva .part")
    got = os.path.getsize(part); check(0 < got < len(DATA), "parcial entre 0 y total")
    LOG.clear()
    r, part, final = probe("/movie.mp4", tmp, "resume")
    check(r["status"] == OK and sha(final) == DATA_SHA, "reanuda y el archivo final es identico")
    check(LOG[0][1] == "bytes=%d-" % got, "pidio Range desde el tamano del parcial")

    # 3) pausa
    r, part, final = probe("/slow.mp4", tmp, "pause", "--pause-at", str(512 * 1024))
    check(r["status"] == PAUSED and os.path.exists(part), "pausa conserva parcial")

    # 4) servidor sin soporte de Range: reinicia una sola vez desde cero
    part = os.path.join(tmp, "nr.part"); final = os.path.join(tmp, "nr.mp4")
    open(part, "wb").write(DATA[:1000])
    out = subprocess.run([PROBE, BASE + "/norange.mp4", part, final], capture_output=True, text=True, timeout=60)
    r = json.loads(out.stdout.strip().splitlines()[-1])
    check(r["status"] == OK and sha(final) == DATA_SHA, "sin Range: reinicia y completa")

    # 5) parcial que ya era el archivo completo (416)
    part = os.path.join(tmp, "done.part"); final = os.path.join(tmp, "done.mp4")
    open(part, "wb").write(DATA)
    out = subprocess.run([PROBE, BASE + "/movie.mp4", part, final], capture_output=True, text=True, timeout=60)
    r = json.loads(out.stdout.strip().splitlines()[-1])
    check(r["status"] == OK and sha(final) == DATA_SHA, "416 sobre parcial completo se da por terminado")

    # 6) HLS: por extension y por contenido
    r, part, final = probe("/list.m3u8", tmp, "hls")
    check(r["status"] == HLS and not os.path.exists(final), "HLS por extension rechazado")
    r, part, final = probe("/list.m3u8?token=1", tmp, "hls2")
    check(r["status"] == HLS, "HLS con consulta rechazado")
    # el mismo contenido servido bajo otra ruta: se detecta por el cuerpo
    class H2(Handler):
        def do_GET(self):
            b = b"#EXTM3U\n#EXTINF:5,\na.ts\n"; self._send(200, b, "video/mp4"); self.wfile.write(b)
    s2 = Server(("127.0.0.1", 0), H2); threading.Thread(target=s2.serve_forever, daemon=True).start()
    out = subprocess.run([PROBE, "http://127.0.0.1:%d/video" % s2.server_address[1], os.path.join(tmp, "b.part"), os.path.join(tmp, "b.mp4")], capture_output=True, text=True, timeout=60)
    r = json.loads(out.stdout.strip().splitlines()[-1]); check(r["status"] == HLS and not os.path.exists(os.path.join(tmp, "b.part")), "HLS por contenido rechazado y sin parcial")
    s2.shutdown()

    # 7) pagina de error 200 y 404
    r, part, final = probe("/error.mp4", tmp, "err")
    check(r["status"] == INVALID and r["refresh"] == 1 and not os.path.exists(final), "HTML de error no se guarda y pide nueva URL")
    r, part, final = probe("/gone.mp4", tmp, "gone")
    check(r["status"] == HTTP and r["http"] == 404 and r["refresh"] == 1 and r["retry"] == 1, "404 pide nueva URL")

    # 8) corte a mitad de camino: queda parcial y se puede reanudar
    r, part, final = probe("/cut.mp4", tmp, "cut")
    check(r["status"] == NETWORK and r["retry"] == 1, "corte de red es reintentable")
    r, part, final = probe("/cut.mp4", tmp, "cut")
    check(r["status"] == OK and os.path.getsize(final) == 1024 * 1024 and sha(final) == hashlib.sha256(DATA[:1024 * 1024]).hexdigest(), "tras el corte reanuda y queda completo")

    # 9) cabeceras de autorizacion: se envian y no se siguen redirecciones con ellas
    r, part, final = probe("/auth.mp4", tmp, "noauth")
    check(r["status"] == HTTP and r["http"] == 403, "sin cabecera: 403")
    r, part, final = probe("/auth.mp4", tmp, "auth", "--header", "Content-Auth:ok")
    check(r["status"] == OK and os.path.getsize(final) == 200000, "con cabecera: ok")
    r, part, final = probe("/redir.mp4", tmp, "redir")
    check(r["status"] == OK and sha(final) == DATA_SHA, "sigue redireccion sin cabeceras")

    # 10) espacio insuficiente (margen enorme)
    r, part, final = probe("/movie.mp4", tmp, "space", "--margin", str(1 << 60))
    check(r["status"] == SPACE and not os.path.exists(final), "falta de espacio se detecta antes de bajar todo")

    # 11) URL no valida
    out = subprocess.run([PROBE, "ftp://x/y.mp4", os.path.join(tmp, "x.part"), os.path.join(tmp, "x.mp4")], capture_output=True, text=True)
    check(json.loads(out.stdout)["status"] == INVALID, "protocolo no http(s) rechazado")

    # ----- gestor de cola -----
    def run_manager(scenario, name):
        d = os.path.join(tmp, name, "descargas"); os.makedirs(d)
        out = subprocess.run([MANAGER, BASE, d, scenario], capture_output=True, text=True, timeout=90)
        check(out.returncode == 0, "manager " + scenario + " " + out.stderr)
        return d, dict(l.split("=", 1) for l in out.stdout.splitlines() if "=" in l and l.split("=", 1)[0] not in ("event", "entry", "sub")), out.stdout.splitlines()

    d, kv, lines = run_manager("nosession", "ns")
    check(kv["nosession"] == "5", "sin sesion no encola (NoSession)")

    d, kv, lines = run_manager("basic", "basic")
    check(kv["r1"] == "0" and kv["r2"] == "0" and kv["dup"] == "2" and kv["movie"] == "0" and kv["notep"] == "4", "resultados de enqueue (nuevo, duplicado, no soportado)")
    check(kv["idle"] == "1" and kv["again"] == "3", "termina y no vuelve a encolar lo descargado")
    files = sorted(os.listdir(d))
    check(sum(f.endswith(".mp4") for f in files) == 3 and not any(f.endswith(".part") for f in files), "tres videos y ningun .part")
    check(all(re.fullmatch(r"[a-z0-9.\-]+", f) for f in files), "nombres de archivo ASCII seguros para FAT: %s" % files)
    check(any(f.startswith("mi-serie-nandu-episodio-1-piloto-") for f in files), "nombre legible sin acentos")
    check(sum(f.endswith(".srt") for f in files) == 6, "dos subtitulos por video (el que falla se omite)")
    entries = [l[len("entry="):].split("|") for l in lines if l.startswith("entry=")]
    check([e[0] for e in entries] == ["e1", "e2", "m1"] or [e[0] for e in entries] == ["m1", "e1", "e2"], "orden de episodios dentro de la serie: %s" % entries)
    check(any(e[1] == "Mi Serie: Ñandú - Episodio 1 - Piloto" for e in entries) and any(e[1] == "Una Pelicula" for e in entries), "titulos: serie + episodio, o solo la pelicula")
    check(all(e[3].startswith("DESCARGADO") for e in entries) and all(e[2].startswith(d) for e in entries), "estado y ruta local")
    check(sum(l.startswith("event=Descarga completa") for l in lines) == 3, "tres avisos de descarga completa")
    lib = json.load(open(os.path.join(d, "..", "offline_library.json")))
    text = json.dumps(lib)
    check(len(lib["items"]) == 3 and "http" not in text and "Content-Auth" not in text, "biblioteca sin URL ni cabeceras")
    check(any(l.startswith("subs=2 ") for l in lines), "dos subtitulos registrados")

    d, kv, lines = run_manager("failures", "fail")
    check(kv["idle"] == "1", "las fallidas no bloquean la cola")
    ev = [l for l in lines if l.startswith("event=")]
    check(len(ev) == 4 and all("fallida" in e for e in ev), "cuatro avisos de fallo")
    check(any("HLS" in e for e in ev) and any("404" in e for e in ev) and any("servicio no entrego" in e for e in ev), "motivos claros")
    check(not any(f.endswith(".part") and "hls" in f for f in os.listdir(d)), "sin restos de la HLS")
    check(kv["retry"] == "1" and kv["cancel_failed"] == "1", "reintentar y descartar una fallida")
    check(not any(f.endswith(".srt") for f in os.listdir(d)), "no quedan subtitulos huerfanos de descargas fallidas")

    d, kv, lines = run_manager("retry_cut", "cut")
    check(kv["idle"] == "1", "termina")
    check(any(l.startswith("downloaded=1") for l in lines), "tras un corte reintenta sola y completa")

    d, kv, lines = run_manager("auth", "auth")
    check(any(l.startswith("downloaded=1") for l in lines), "el gestor entrega las cabeceras de la fuente")

    d, kv, lines = run_manager("ext", "ext")
    check(kv["file"].endswith(".mkv"), "extension tomada de la URL")

    d, kv, lines = run_manager("cancel_pause", "cp")
    check(kv["active"].startswith("DESCARGANDO"), "estado activo")
    check(kv["paused"].startswith("EN PAUSA|Descargas en pausa"), "pausa visible en la tarjeta y en la barra")
    check(kv["resumed"].startswith("DESCARGANDO") and kv["overlay"].startswith("Descargando 1/2"), "reanuda sola al quitar la pausa")
    check(kv["cancel_queued"] == "1" and kv["cancel_active"] == "1" and kv["idle"] == "1", "cancelar en cola y en curso")
    check(kv["left"] == "0 downloaded=0", "no queda nada")
    check(not any(f.endswith(".part") for f in os.listdir(d)), "cancelar borra el parcial")
    check(any(l.startswith("event=Descarga cancelada") for l in lines), "aviso de cancelacion")

    d, kv, lines = run_manager("remove", "rm")
    check(kv["before"] == "1" and kv["removed"] == "1 after=0", "borrar descarga")
    check(os.listdir(d) == [], "borrar elimina video y subtitulos de la SD: %s" % os.listdir(d))
finally:
    srv.shutdown(); shutil.rmtree(tmp, ignore_errors=True)
print("descargas offline: %d comprobaciones aprobadas" % passed)
