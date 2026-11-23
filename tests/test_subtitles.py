#!/usr/bin/env python3
"""Pruebas de la busqueda de subtitulos con servidores simulados (OpenSubtitles y SubDL)."""
import io, json, subprocess, sys, tempfile, threading, urllib.parse, zipfile, os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROBE = sys.argv[1]
SRT = "1\n00:00:01,000 --> 00:00:02,000\nHola mundo\n"
state = {"os_mode": "ok", "sd_mode": "ok", "log": []}

def make_zip(files, method=zipfile.ZIP_DEFLATED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, data in files: z.writestr(name, data)
    return buf.getvalue()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(data))); self.end_headers()
        try: self.wfile.write(data)
        except Exception: pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode()
        state["log"].append(("POST", self.path, dict(self.headers), body))
        if self.path == "/os/login":
            if self.headers.get("Api-Key") != "OSKEY": return self.reply(401, {})
            return self.reply(200, {"token": "JWT123", "status": 200})
        if self.path == "/os/download":
            if state["os_mode"] == "quota": return self.reply(406, {"message": "limit"})
            port = self.server.server_address[1]
            return self.reply(200, {"link": f"http://127.0.0.1:{port}/files/a.srt", "remaining": 4})
        self.reply(404, {})
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        state["log"].append(("GET", self.path, dict(self.headers), ""))
        if u.path == "/os/subtitles":
            m = state["os_mode"]
            if m == "401": return self.reply(401, {})
            if m == "429": return self.reply(429, {})
            if m == "empty": return self.reply(200, {"data": []})
            if m == "junk": return self.reply(200, b"<html>x</html>", "text/html")
            data = [
                {"id": "1", "attributes": {"language": "en", "download_count": 900, "release": "Show.S01E02.1080p", "files": [{"file_id": 111, "file_name": "a.srt"}], "feature_details": {"season_number": 1, "episode_number": 2}}},
                {"id": "2", "attributes": {"language": "es", "download_count": 50, "release": "Show.S01E02.WEB", "files": [{"file_id": 222, "file_name": "b.srt"}], "feature_details": {"season_number": 1, "episode_number": 2}}},
                {"id": "3", "attributes": {"language": "es", "download_count": 500, "release": "Show.S01E02.HDTV", "files": [{"file_id": 333, "file_name": "c.srt"}], "feature_details": {"season_number": 1, "episode_number": 2}}},
                {"id": "4", "attributes": {"language": "es", "download_count": 9999, "release": "Show.S01E09", "files": [{"file_id": 444, "file_name": "d.srt"}], "feature_details": {"season_number": 1, "episode_number": 9}}},
                {"id": "5", "attributes": {"language": "es", "files": [{"file_id": "../x"}]}},
                {"id": "6", "attributes": {"language": "fr", "download_count": 1, "files": [{"file_id": 666}]}},
            ]
            return self.reply(200, {"data": data})
        if u.path == "/files/a.srt": return self.reply(200, SRT.encode(), "text/plain")
        if u.path == "/sd/subtitles":
            m = state["sd_mode"]
            if m == "empty": return self.reply(200, {"status": True, "results": [], "subtitles": []})
            if m == "error": return self.reply(200, {"status": False, "error": "api key invalida"})
            subs = [
                {"release_name": "Show.S01E02.WEB", "lang": "ES", "language": "ES", "url": "/subtitle/100-200.zip", "season": 1, "episode": 2},
                {"release_name": "evil", "language": "ES", "url": "https://evil.example/x.zip"},
                {"release_name": "evil2", "language": "ES", "url": "/subtitle/../../etc.zip"},
                {"release_name": "Ep3", "language": "EN", "url": "/subtitle/300-400.zip", "season": 1, "episode": 3},
                {"release_name": "Show.English", "language": "EN", "url": "/subtitle/101-201.zip", "season": 1, "episode": 2},
            ]
            return self.reply(200, {"status": True, "results": [{"name": "Show"}], "subtitles": subs})
        if u.path.startswith("/dl/subtitle/"):
            name = u.path.split("/")[-1]
            if name == "100-200.zip":
                return self.reply(200, make_zip([("readme.txt", "x"), ("Show.S01E02.srt", SRT), ("__MACOSX/._a.srt", "bad")]), "application/zip")
            if name == "101-201.zip": return self.reply(200, make_zip([("a.srt", SRT)], zipfile.ZIP_STORED), "application/zip")
            return self.reply(404, {})
        self.reply(404, {})

srv = ThreadingHTTPServer(("127.0.0.1", 0), H); port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
failures = []
def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  -> " + str(extra)))
    if not cond: failures.append(name)
def run(*args):
    return subprocess.run([PROBE, *map(str, args)], capture_output=True, text=True, timeout=60).stdout
def search(keys, title="Show", season=1, episode=2, idx=None):
    state["log"].clear()
    a = ["search", port, keys, title, season, episode] + ([idx] if idx is not None else [])
    return run(*a)

# --- adivinar titulo / temporada / episodio
cases = {
    "Show S02E05 - Titulo (2019)": "title=[Show] season=2 episode=5 year=2019",
    "Mi.Serie.2x07.HDTV": "title=[Mi Serie] season=2 episode=7 year=0",
    "Reproducir: La Pelicula": "title=[La Pelicula] season=0 episode=0 year=0",
    "Serie - Episodio 3 - El comienzo": "title=[Serie] season=0 episode=3 year=0",
    "Serie Temporada 2 - Episodio 10": "title=[Serie] season=2 episode=10 year=0",
    "Pelicula [1080p] (2021)": "title=[Pelicula] season=0 episode=0 year=2021",
    "Season 4": "title=[] season=4 episode=0 year=0",
    "Capital Episodes": "title=[Capital Episodes] season=0 episode=0 year=0",
}
for src, want in cases.items():
    got = run("guess", src).strip(); check("guess " + src, got == want, got)

# --- zip
z = tempfile.NamedTemporaryFile(suffix=".zip", delete=False); z.write(make_zip([("a.txt", "no"), ("b.vtt", "WEBVTT\n"), ("c.srt", SRT)])); z.close()
out = run("zip", z.name); check("zip prefiere .srt", "first=[1]" in out and f"size={len(SRT)}" in out, out)
z2 = tempfile.NamedTemporaryFile(suffix=".zip", delete=False); z2.write(make_zip([("a.txt", "no")])); z2.close()
check("zip sin subtitulos", "error=" in run("zip", z2.name))
z3 = tempfile.NamedTemporaryFile(suffix=".zip", delete=False); z3.write(b"not a zip at all, definitely not"); z3.close()
check("zip invalido", "error=" in run("zip", z3.name))
z4 = tempfile.NamedTemporaryFile(suffix=".zip", delete=False); z4.write(make_zip([("../evil.srt", SRT)])); z4.close()
check("zip con ruta peligrosa se ignora", "error=" in run("zip", z4.name))
big = tempfile.NamedTemporaryFile(suffix=".zip", delete=False); big.write(make_zip([("big.srt", "a" * (3 * 1024 * 1024))])); big.close()
check("zip enorme se rechaza", "error=" in run("zip", big.name))
trunc = make_zip([("c.srt", SRT * 50)]); t5 = tempfile.NamedTemporaryFile(suffix=".zip", delete=False); t5.write(trunc[:len(trunc) // 2]); t5.close()
check("zip truncado no revienta", "error=" in run("zip", t5.name))

# --- OpenSubtitles primero
state["os_mode"] = "ok"; state["sd_mode"] = "ok"
out = search("os,sd")
check("OS: usa OpenSubtitles", "used=OpenSubtitles" in out and "search=1" in out, out)
hits = [l for l in out.splitlines() if l.startswith("hit ")]
check("OS: descarta episodio equivocado y ids raros", len(hits) == 3 and not any("S01E09" in h for h in hits), hits)
check("OS: espanol antes que ingles, luego por descargas", [h.split("|")[1] for h in hits] == ["es", "es", "en"] and "HDTV" in hits[0], hits)
check("OS: fetch ok", "fetch=1" in out and f"size={len(SRT)}" in out and "first=[1]" in out, out)
gets = [e for e in state["log"] if e[1].startswith("/os/subtitles")]
check("OS: cabeceras Api-Key y User-Agent", gets and gets[0][2].get("Api-Key") == "OSKEY" and "XuperSwitch" in gets[0][2].get("User-Agent", ""), gets and gets[0][2])
q = urllib.parse.parse_qs(urllib.parse.urlparse(gets[0][1]).query)
check("OS: parametros", q.get("query") == ["show"] and q.get("season_number") == ["1"] and q.get("episode_number") == ["2"] and q.get("type") == ["episode"] and q.get("languages") == ["es,en"], q)
dl = [e for e in state["log"] if e[1] == "/os/download"]
check("OS: login y token en la descarga", dl and dl[0][2].get("Authorization") == "Bearer JWT123" and json.loads(dl[0][3]) == {"file_id": 333}, dl and (dl[0][2], dl[0][3]))
filed = [e for e in state["log"] if e[1] == "/files/a.srt"]
check("OS: el enlace temporal se pide sin claves", filed and "Api-Key" not in filed[0][2] and "Authorization" not in filed[0][2], filed and filed[0][2])
out = search("nologin")
dl = [e for e in state["log"] if e[1] == "/os/download"]
check("OS sin usuario: descarga sin Authorization", "fetch=1" in out and dl and "Authorization" not in dl[0][2], out)
check("OS: elegir otro resultado", "file_id\": 222" in json.dumps(json.loads(search("os", idx=1) and [e for e in state["log"] if e[1] == "/os/download"][0][3])).replace('"file_id": 222', 'file_id": 222'), state["log"][-3:])

# --- respaldo
for mode in ("empty", "401", "429", "junk"):
    state["os_mode"] = mode
    out = search("os,sd")
    check(f"respaldo a SubDL cuando OS = {mode}", "used=SubDL" in out and "search=1" in out, out)
state["os_mode"] = "empty"
out = search("os,sd")
hits = [l for l in out.splitlines() if l.startswith("hit ")]
check("SubDL: filtra urls ajenas y otro episodio", len(hits) == 2 and all("SubDL" in h for h in hits) and not any("Ep3" in h for h in hits), hits)
check("SubDL: espanol primero", hits and hits[0].split("|")[1] == "es", hits)
check("SubDL: fetch descomprime el zip", "fetch=1" in out and "first=[1]" in out, out)
sget = [e for e in state["log"] if e[1].startswith("/sd/subtitles")][0]
sq = urllib.parse.parse_qs(urllib.parse.urlparse(sget[1]).query)
check("SubDL: parametros", sq.get("film_name") == ["Show"] and sq.get("type") == ["tv"] and sq.get("season_number") == ["1"] and sq.get("episode_number") == ["2"] and sq.get("languages") == ["ES,EN"] and sq.get("api_key") == ["SDKEY"], sq)
dlg = [e for e in state["log"] if e[1].startswith("/dl/")]
check("SubDL: descarga sin enviar la clave", dlg and "SDKEY" not in dlg[0][1] and "Api-Key" not in dlg[0][2], dlg)
out = search("os,sd", idx=1)
check("SubDL: zip sin comprimir (stored)", "fetch=1" in out, out)
state["os_mode"] = "empty"; state["sd_mode"] = "empty"
out = search("os,sd")
check("sin resultados en ninguna fuente", "search=0" in out and "sin resultados" in out and "OpenSubtitles" in out and "SubDL" in out, out)
state["os_mode"] = "401"; state["sd_mode"] = "error"
out = search("os,sd")
check("errores de ambas fuentes se informan", "search=0" in out and "clave o usuario" in out and "invalida" in out, out)
check("el error no incluye las claves", "OSKEY" not in out and "SDKEY" not in out and "pass" not in out.replace("passed", ""), out)
state["os_mode"] = "ok"; state["sd_mode"] = "ok"
out = search("sd")
check("solo SubDL configurado", "used=SubDL" in out and not [e for e in state["log"] if e[1].startswith("/os/")], out)
out = search("os")
check("solo OpenSubtitles configurado", "used=OpenSubtitles" in out and not [e for e in state["log"] if e[1].startswith("/sd/")], out)
out = search("none")
check("sin claves: mensaje claro", "search=0" in out and "LEEME-SUBTITULOS" in out, out)
state["os_mode"] = "quota"
out = search("os")
check("limite diario de descargas", "fetch=0" in out and "limite de descargas" in out, out)
state["os_mode"] = "ok"
out = run("search", 1, "os,sd", "Show", 1, 2)   # puerto sin servidor
check("sin conexion: error, no cuelga", "search=0" in out and "Conexion" in out, out)
out = search("os,sd", title="")
check("titulo vacio", "search=0" in out, out)
# https obligatorio sin allow_http: se prueba contra la API real sin red -> no debe aceptar http
print("\n%d fallos" % len(failures)); sys.exit(1 if failures else 0)
