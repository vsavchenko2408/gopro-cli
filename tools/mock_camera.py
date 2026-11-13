#!/usr/bin/env python3
"""A fake GoPro HERO7 Black for testing gopro-cli without the camera.

    python3 tools/mock_camera.py            # listens on 127.0.0.1:8099
    export GOPRO_HOST=127.0.0.1 GOPRO_PORT=8099 GOPRO_MEDIA_PORT=8099
    ./build/gopro status

It serves the real settings schema captured from a HERO7 Black and keeps
settings, mode and recording state in memory. It imitates the protocol only;
it does not enforce which setting combinations the real camera allows.
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

SCHEMA = json.loads((Path(__file__).parent / "hero7black-schema.json").read_text())

status = {"1": 1, "2": 3, "8": 0, "10": 0, "13": 0, "30": "GP24784461", "33": 0, "34": 4155,
          "35": 6542, "38": 3, "39": 4, "43": 0, "44": 0, "54": 27655117, "70": 84}
settings = {str(s["id"]): s["options"][0]["value"]
            for m in SCHEMA["modes"] for s in m["settings"] if s["options"]}
settings.update({"2": 9, "3": 8, "4": 0})  # 1080p, 30 fps, wide
media = {"100GOPRO": {"GH010001.MP4": b"\x00" * 3_000_000, "GOPR0002.JPG": b"\xff" * 250_000}}
record_started = 0.0
busy_until = 0.0  # like the real camera: busy for a moment after a recording stops


class Handler(BaseHTTPRequestHandler):
    def send(self, body, content_type="application/json", code=200):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):  # noqa: N802 (name required by http.server)
        global record_started, busy_until
        url = urlparse(self.path)
        query = {k: v[0] for k, v in parse_qs(url.query).items()}
        path = url.path.rstrip("/")

        if path == "/gp/gpControl":
            return self.send(SCHEMA)
        busy = time.time() < busy_until
        status["8"] = 1 if busy else 0
        if busy and path == "/gp/gpMediaList":
            return self.send(b"busy", "text/plain", 503)
        if busy and path.startswith("/gp/gpControl/command/mode"):
            return self.send(b"busy", "text/plain", 500)
        if path == "/gp/gpControl/status":
            if status["10"]:
                status["13"] = int(time.time() - record_started)
            return self.send({"status": status, "settings": settings})
        if path.startswith("/gp/gpControl/setting/"):
            setting_id, value = path.split("/")[-2:]
            settings[setting_id] = int(value)
            return self.send({})
        if path == "/gp/gpControl/command/shutter":
            on = query.get("p") == "1"
            if status["43"] == 0:  # video: shutter toggles recording
                status["10"] = 1 if on else 0
                record_started = time.time()
                if not on:
                    status["39"] += 1
                    busy_until = time.time() + 2.0
            elif on:
                status["38"] += 1
            return self.send({})
        if path == "/gp/gpControl/command/mode":
            status["43"], status["44"] = int(query["p"]), 0
            return self.send({})
        if path == "/gp/gpControl/command/sub_mode":
            status["43"], status["44"] = int(query["mode"]), int(query["sub_mode"])
            return self.send({})
        if path == "/gp/gpControl/command/storage/delete":
            directory, name = query["p"].strip("/").split("/")
            if media.get(directory, {}).pop(name, None) is None:
                return self.send({"error": "no such file"}, code=404)
            return self.send({})
        if path.startswith("/gp/gpControl/command/") or path == "/gp/gpControl/execute":
            return self.send({})  # tag, locate, zoom, sleep, date_time, pair...
        if path == "/gp/gpMediaList":
            now = str(int(time.time()))
            return self.send({"id": "mock", "media": [
                {"d": d, "fs": [{"n": n, "cre": now, "mod": now, "s": str(len(data))}
                                for n, data in files.items()]}
                for d, files in media.items()]})
        if path.startswith("/videos/DCIM/"):
            directory, name = path.split("/")[-2:]
            data = media.get(directory, {}).get(name)
            if data is None:
                return self.send(b"not found", "text/plain", 404)
            return self.send(data, "application/octet-stream")
        return self.send(b"not found", "text/plain", 404)

    def log_message(self, fmt, *args):
        sys.stderr.write("mock camera: " + fmt % args + "\n")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    print(f"Mock HERO7 Black on http://127.0.0.1:{port}", file=sys.stderr)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
