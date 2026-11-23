#!/usr/bin/env python3
"""Ejecuta la app (SDL 'dummy') sin conexion con una descarga guardada; no usa red."""
import json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

def main(binary):
    root = Path(__file__).resolve().parent.parent
    runtime = root / "packaging" / "runtime" / "switch" / "xuperswitch"
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp); (folder / "descargas").mkdir()
        shutil.copy(runtime / "font.ttf", folder / "font.ttf")
        video = folder / "descargas" / "prueba-12345678.mp4"; shutil.copy(runtime / "test.mp4", video)
        sub = folder / "descargas" / "prueba-12345678.1.srt"; sub.write_text("1\n00:00:00,000 --> 00:00:03,000\nHola\n")
        (folder / "offline_library.json").write_text(json.dumps({"items": [{
            "id": "m1", "series": "", "type": "1", "title": "Una Pelicula", "file": str(video), "art": "", "size": video.stat().st_size,
            "order": 0, "saved": 1, "subs": [["Espanol", str(sub)]]}]}))
        # un archivo que no existe: debe limpiarse del registro al arrancar
        lib = json.loads((folder / "offline_library.json").read_text())
        lib["items"].append({"id": "gone", "series": "", "type": "1", "title": "Borrada a mano", "file": str(folder / "descargas" / "no-existe.mp4"), "art": "", "size": 1, "order": 0, "saved": 0, "subs": []})
        (folder / "offline_library.json").write_text(json.dumps(lib))
        env = dict(os.environ, SDL_VIDEODRIVER="dummy")
        subprocess.run([str(Path(binary).resolve())], cwd=folder, env=env, check=True, timeout=120)
        assert not video.exists() and not sub.exists(), "el doble ZR debe borrar video y subtitulos de la SD"
        assert json.loads((folder / "offline_library.json").read_text())["items"] == [], "la biblioteca queda vacia"
        recent = json.loads((folder / "library_state.json").read_text())["recent"]
        assert recent and recent[0]["kind"] == "episode" and recent[0]["content"] == "m1" and "source" not in recent[0]
        print("App sin conexion: arranque offline, limpieza de huerfanos, reproduccion local, Continuar viendo y borrado verificados")
main(sys.argv[1])
