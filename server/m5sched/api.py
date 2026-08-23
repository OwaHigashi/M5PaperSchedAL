"""Plain-HTTP REST API for the M5Paper + a small dashboard for humans.

Device endpoints (no TLS, Content-Length always set, no chunking):
  GET  /api/v1/events            NDJSON: header line + one event per line
  POST /api/v1/heartbeat         active sensing  -> {now, rev, cmds[], ...}
  POST /api/v1/alarm/ack         {alarm_id, result}
  POST /api/v1/cmd/ack           {id, ok, info}
  GET  /api/v1/midi/<name>       MIDI bytes (downloaded from midi_url by the host, cached)
  POST /api/v1/screenshot        raw 4bpp 540x960 frame buffer -> PNG on the host
  GET  /api/v1/time              {now, tz}

Human endpoints:
  GET  /                         dashboard
  GET  /api/v1/status            JSON
  GET  /api/v1/list              events with alarm state (JSON)
  POST /api/v1/cmd               enqueue a command for the device
  POST /api/v1/refresh           re-fetch ICS now
  GET  /screenshots/<file>
  GET  /log/<alarm|device>
"""
import datetime as dt
import json
import logging
import os
import re
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import requests

from . import __version__
from .dashboard import DASHBOARD_HTML

log = logging.getLogger("api")

SAFE_NAME = re.compile(r"^[A-Za-z0-9_.\-]{1,63}$")


class MidiCache:
    def __init__(self, cfg):
        self.cfg = cfg
        self.dir = os.path.join(cfg.cache_dir, "midi")
        self.local_dir = os.path.join(cfg.base_dir, "midi")   # server/midi/*.mid shipped with the repo
        self.lock = threading.Lock()

    def get(self, name: str) -> bytes | None:
        if not SAFE_NAME.match(name) or not name.lower().endswith((".mid", ".midi")):
            return None
        for d in (self.local_dir, self.dir):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    return f.read()
        base = self.cfg.get("midi_url") or ""
        if not base:
            return None
        url = base.rstrip("/") + "/" + urllib.parse.quote(name)
        with self.lock:
            try:
                r = requests.get(url, timeout=20)
                r.raise_for_status()
                if r.content[:4] != b"MThd":
                    raise ValueError("not a MIDI file")
                p = os.path.join(self.dir, name)
                with open(p + ".tmp", "wb") as f:
                    f.write(r.content)
                os.replace(p + ".tmp", p)
                log.info("MIDI cached: %s (%d bytes)", name, len(r.content))
                return r.content
            except Exception as e:  # noqa: BLE001
                log.warning("MIDI download failed %s: %s", url, e)
                return None

    def list(self):
        names = set()
        for d in (self.local_dir, self.dir):
            if os.path.isdir(d):
                names.update(n for n in os.listdir(d) if n.lower().endswith((".mid", ".midi")))
        return sorted(names)


def make_handler(sched, cfg, midi_cache):
    token = cfg.get("api_token") or ""

    class Handler(BaseHTTPRequestHandler):
        server_version = f"m5sched/{__version__}"
        protocol_version = "HTTP/1.1"

        # ---- helpers -------------------------------------------------
        def log_message(self, fmt, *args):  # quieter access log
            if self.path.startswith("/api/v1/heartbeat"):
                return
            log.debug("%s %s", self.address_string(), fmt % args)

        def _send(self, code, body: bytes, ctype="application/json; charset=utf-8", extra=None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, code=200):
            self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

        def _read_body(self, limit=2 * 1024 * 1024) -> bytes:
            n = int(self.headers.get("Content-Length", "0") or 0)
            if n < 0 or n > limit:
                raise ValueError("body too large")
            data = b""
            while len(data) < n:
                chunk = self.rfile.read(min(65536, n - len(data)))
                if not chunk:
                    break
                data += chunk
            return data

        def _read_json(self) -> dict:
            raw = self._read_body()
            if not raw:
                return {}
            return json.loads(raw.decode("utf-8", errors="replace"))

        def _authed(self) -> bool:
            if not token:
                return True
            return self.headers.get("X-Token", "") == token

        def _client(self):
            return self.client_address[0]

        # ---- GET -----------------------------------------------------
        def do_GET(self):
            try:
                path = urllib.parse.urlsplit(self.path).path
                if path == "/" or path == "/index.html":
                    return self._send(200, DASHBOARD_HTML.encode("utf-8"), "text/html; charset=utf-8")
                if path == "/api/v1/status":
                    return self._json(sched.status())
                if path == "/api/v1/list":
                    return self._json({"rev": sched.rev, "events": sched.events_for_dashboard()})
                if path == "/api/v1/midi":
                    return self._json({"files": midi_cache.list()})
                if path == "/api/v1/time":
                    return self._json({"now": round(dt.datetime.now().timestamp(), 3), "tz": cfg["tz_posix"]})
                if not self._authed():
                    return self._json({"error": "unauthorized"}, 401)
                if path == "/api/v1/events":
                    return self._events()
                if path.startswith("/api/v1/midi/"):
                    name = urllib.parse.unquote(path[len("/api/v1/midi/"):])
                    data = midi_cache.get(name)
                    if data is None:
                        return self._json({"error": "not found"}, 404)
                    return self._send(200, data, "audio/midi")
                if path.startswith("/screenshots/"):
                    name = path[len("/screenshots/"):]
                    p = os.path.join(cfg.data_dir, "screenshots", name)
                    if not SAFE_NAME.match(name) or not os.path.isfile(p):
                        return self._json({"error": "not found"}, 404)
                    with open(p, "rb") as f:
                        return self._send(200, f.read(), "image/png")
                if path in ("/log/alarm", "/log/device"):
                    p = sched.log_path if path.endswith("alarm") else sched.device_log_path
                    data = b""
                    if os.path.isfile(p):
                        with open(p, "rb") as f:
                            f.seek(0, 2)
                            size = f.tell()
                            f.seek(max(0, size - 200_000))
                            data = f.read()
                    return self._send(200, data, "text/plain; charset=utf-8")
                return self._json({"error": "not found"}, 404)
            except Exception as e:  # noqa: BLE001
                log.exception("GET %s failed", self.path)
                try:
                    self._json({"error": str(e)}, 500)
                except Exception:  # noqa: BLE001
                    pass

        def _events(self):
            if not sched.loaded:
                return self._json({"error": "table not ready"}, 503)
            header = sched.device_header()
            lines = [json.dumps(header, ensure_ascii=False, separators=(",", ":"))]
            for ev in sched.device_event_lines():
                lines.append(json.dumps(ev, ensure_ascii=False, separators=(",", ":")))
            lines.append('{"type":"end"}')
            body = ("\n".join(lines) + "\n").encode("utf-8")
            self._send(200, body, "application/x-ndjson; charset=utf-8", {"X-Rev": str(header["rev"])})

        # ---- POST ----------------------------------------------------
        def do_POST(self):
            try:
                path = urllib.parse.urlsplit(self.path).path
                if path == "/api/v1/refresh":
                    sched.request_fetch()
                    return self._json({"ok": True})
                if path == "/api/v1/cmd":
                    body = self._read_json()
                    cmd = str(body.get("cmd", ""))
                    if cmd not in ("refresh", "reboot", "play", "stop", "screenshot", "message",
                                   "show", "redraw", "ping", "config"):
                        return self._json({"error": "unknown cmd"}, 400)
                    cid = sched.device.enqueue(body, source=f"web:{self._client()}")
                    return self._json({"ok": True, "id": cid})
                if not self._authed():
                    return self._json({"error": "unauthorized"}, 401)
                if path == "/api/v1/heartbeat":
                    body = self._read_json()
                    return self._json(sched.device_heartbeat(body, self._client()))
                if path == "/api/v1/alarm/ack":
                    return self._json(sched.device_alarm_ack(self._read_json()))
                if path == "/api/v1/cmd/ack":
                    body = self._read_json()
                    sched.device.ack_cmd(int(body.get("id", 0)), body.get("ok", True), body.get("info", ""))
                    return self._json({"ok": True})
                if path == "/api/v1/log":
                    body = self._read_json()
                    sched.device_log(body.get("lines") or [])
                    return self._json({"ok": True})
                if path == "/api/v1/screenshot":
                    return self._screenshot()
                return self._json({"error": "not found"}, 404)
            except Exception as e:  # noqa: BLE001
                log.exception("POST %s failed", self.path)
                try:
                    self._json({"error": str(e)}, 500)
                except Exception:  # noqa: BLE001
                    pass

        def _screenshot(self):
            w = int(self.headers.get("X-Width", "540"))
            h = int(self.headers.get("X-Height", "960"))
            raw = self._read_body(limit=4 * 1024 * 1024)
            if len(raw) < w * h // 2:
                return self._json({"error": f"bad size {len(raw)}"}, 400)
            raw = raw[: w * h // 2]      # M5EPD reports one extra byte
            name = dt.datetime.now().strftime("ss_%Y%m%d_%H%M%S.png")
            p = os.path.join(cfg.data_dir, "screenshots", name)
            try:
                from PIL import Image
                px = bytearray(w * h)
                for i, b in enumerate(raw):
                    px[2 * i] = 17 * (15 - (b >> 4))
                    px[2 * i + 1] = 17 * (15 - (b & 0x0F))
                Image.frombytes("L", (w, h), bytes(px)).save(p)
            except ImportError:
                p = p[:-4] + ".pgm"
                with open(p, "wb") as f:
                    f.write(f"P5 {w} {h} 255 ".encode())
                    for b in raw:
                        f.write(bytes((17 * (15 - (b >> 4)), 17 * (15 - (b & 0x0F)))))
            log.info("screenshot saved: %s", p)
            return self._json({"ok": True, "file": os.path.basename(p)})

    return Handler


def serve(sched, cfg):
    midi_cache = MidiCache(cfg)
    handler = make_handler(sched, cfg, midi_cache)
    httpd = ThreadingHTTPServer((cfg["listen"], int(cfg["port"])), handler)
    httpd.daemon_threads = True
    httpd.request_queue_size = 32
    log.info("listening on %s:%d", cfg["listen"], cfg["port"])
    return httpd
