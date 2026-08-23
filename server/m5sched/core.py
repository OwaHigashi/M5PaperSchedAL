"""Scheduler core: event table, alarm state machine, device supervision.

Trust model
-----------
* The host is the single source of truth for events, alarm times and the
  "already rang" flags.  They are persisted in data/state.json.
* The device is untrusted.  It sends heartbeats (active sensing) and acks.
  The host cross-checks: clock skew, reboot (uptime reset), firmware version,
  table revision, and whether an alarm that was due actually got acked.
* If the device stays silent the host still notifies via ntfy, so a dead
  panel never makes an appointment vanish silently.
"""
import datetime as dt
import json
import logging
import os
import threading
import time
from zoneinfo import ZoneInfo

from . import ics as icsmod

log = logging.getLogger("core")


def _now():
    return time.time()


class DeviceState:
    """What we know about the (single) M5Paper."""

    def __init__(self):
        self.lock = threading.Lock()
        self.last_seen = 0.0
        self.first_seen = 0.0
        self.online = False
        self.addr = ""
        self.info = {}            # last heartbeat body (sanitised)
        self.skew_sec = 0.0
        self.uptime_sec = 0
        self.reboots = 0
        self.heartbeats = 0
        self.last_seq = -1
        self.seq_gaps = 0
        self.anomalies = []       # (ts, text) ring
        self.rev = -1
        self.fw = ""
        self.playing = None
        self.cmd_queue = []       # pending commands for the device
        self.cmd_seq = int(_now())   # monotonic across server restarts (device dedups by id)
        self.cmd_history = []     # (ts, cmd, ack)
        self.ui_events = []       # recent touch/button events reported by the device

    def anomaly(self, text):
        self.anomalies.append((_now(), text))
        self.anomalies = self.anomalies[-100:]
        log.warning("DEVICE anomaly: %s", text)

    def enqueue(self, cmd: dict, source="host"):
        with self.lock:
            self.cmd_seq += 1
            c = dict(cmd)
            c["id"] = self.cmd_seq
            c["ts"] = int(_now())
            c["src"] = source
            self.cmd_queue.append(c)
            self.cmd_history.append({"cmd": c, "ack": None})
            self.cmd_history = self.cmd_history[-200:]
            return c["id"]

    def ack_cmd(self, cid, ok, info=""):
        with self.lock:
            for h in self.cmd_history:
                if h["cmd"]["id"] == cid:
                    h["ack"] = {"ok": bool(ok), "info": str(info)[:200], "ts": int(_now())}
            self.cmd_queue = [c for c in self.cmd_queue if c["id"] != cid]

    def pop_commands(self):
        """Commands are delivered with the heartbeat response and kept until acked
        (re-sent every heartbeat; the device dedups by id)."""
        with self.lock:
            return [dict(c) for c in self.cmd_queue[:10]]

    def snapshot(self):
        with self.lock:
            return {
                "online": self.online,
                "last_seen": self.last_seen,
                "first_seen": self.first_seen,
                "addr": self.addr,
                "info": self.info,
                "skew_sec": round(self.skew_sec, 2),
                "uptime_sec": self.uptime_sec,
                "reboots": self.reboots,
                "heartbeats": self.heartbeats,
                "seq_gaps": self.seq_gaps,
                "rev": self.rev,
                "fw": self.fw,
                "playing": self.playing,
                "cmd_queue": self.cmd_queue,
                "cmd_history": self.cmd_history[-30:],
                "anomalies": [{"ts": t, "text": x} for t, x in self.anomalies[-30:]],
                "ui_events": self.ui_events[-50:],
            }


class Scheduler:
    def __init__(self, cfg, notifier):
        self.cfg = cfg
        self.notifier = notifier
        self.tz = ZoneInfo(cfg["tz"])
        self.lock = threading.RLock()
        self.events = []            # current table (list of dicts, sorted)
        self.by_id = {}
        self.rev = 0
        self.last_fetch = 0.0
        self.last_change = 0.0
        self.trimmed = 0
        verify = cfg.get("ics_verify_tls", True)
        if verify and cfg.get("ca_file"):
            verify = cfg._abs(cfg["ca_file"])
        self.sources = [icsmod.IcsSource(i, s, cfg.cache_dir, cfg["ics_timeout_sec"], verify)
                        for i, s in enumerate(cfg["ics_urls"])]
        self.first_load = True
        self.device = DeviceState()
        self.state_path = os.path.join(cfg.data_dir, "state.json")
        self.events_path = os.path.join(cfg.data_dir, "events_cache.json")
        self.loaded = False          # True once a table (fresh or cached) is available
        self.alarm_state = {}       # alarm_id -> dict(triggered, how, at, ts, notified, pushed)
        self._load_state()
        self._stop = threading.Event()
        self._fetch_now = threading.Event()
        self.log_path = os.path.join(cfg.data_dir, "log", "alarm.log")
        self.device_log_path = os.path.join(cfg.data_dir, "log", "device.log")

    # ------------------------------------------------------------------ state
    def _load_state(self):
        try:
            with open(self.state_path, "r", encoding="utf-8") as f:
                d = json.load(f)
            self.alarm_state = d.get("alarm_state", {})
            self.rev = int(d.get("rev", 0))
            log.info("state loaded: %d alarm records, rev=%d", len(self.alarm_state), self.rev)
        except FileNotFoundError:
            self.alarm_state = {}
        except Exception as e:  # noqa: BLE001
            log.error("state load failed: %s", e)
            self.alarm_state = {}
        # last known table: lets the device keep working through a restart / ICS outage
        try:
            with open(self.events_path, "r", encoding="utf-8") as f:
                d = json.load(f)
            self.events = d.get("events", [])
            self.by_id = {e["id"]: e for e in self.events}
            self.last_fetch = float(d.get("saved", 0))
            self.loaded = bool(self.events)
            self.first_load = False    # alarm states already exist; don't re-expire
            log.info("events cache loaded: %d events", len(self.events))
        except FileNotFoundError:
            pass
        except Exception as e:  # noqa: BLE001
            log.error("events cache load failed: %s", e)

    def _save_events(self):
        tmp = self.events_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"events": self.events, "saved": int(_now()), "rev": self.rev}, f, ensure_ascii=False)
        os.replace(tmp, self.events_path)

    def _save_state(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"alarm_state": self.alarm_state, "saved": int(_now()), "rev": self.rev}, f,
                      ensure_ascii=False, indent=1)
        os.replace(tmp, self.state_path)

    def alog(self, fmt, *a):
        line = f"{dt.datetime.now(self.tz).strftime('%m-%d %H:%M:%S')} " + (fmt % a if a else fmt)
        log.info("ALARM %s", fmt % a if a else fmt)
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")

    def device_log(self, lines):
        if not lines:
            return
        stamp = dt.datetime.now(self.tz).strftime('%m-%d %H:%M:%S')
        with open(self.device_log_path, "a", encoding="utf-8") as f:
            for l in lines:
                f.write(f"{stamp} {str(l)[:500]}\n")

    # ------------------------------------------------------------------ fetch
    def start(self):
        threading.Thread(target=self._fetch_loop, daemon=True, name="fetch").start()
        threading.Thread(target=self._tick_loop, daemon=True, name="tick").start()

    def stop(self):
        self._stop.set()

    def request_fetch(self):
        self._fetch_now.set()

    def _fetch_loop(self):
        while not self._stop.is_set():
            try:
                self.refresh()
            except Exception as e:  # noqa: BLE001
                log.exception("refresh failed: %s", e)
            self._fetch_now.wait(timeout=max(10, int(self.cfg["ics_poll_sec"])))
            self._fetch_now.clear()

    def refresh(self):
        bodies = []
        for src in self.sources:
            body = src.fetch()
            if body:
                bodies.append((src.idx, body))
        now = dt.datetime.now(self.tz)
        if not bodies:
            log.error("no ICS source available at all; keeping previous table (%d events)", len(self.events))
            return
        events = icsmod.parse_sources(bodies, self.cfg, now)
        events, trimmed = icsmod.trim_events(events, int(self.cfg["max_events"]), now.timestamp())
        with self.lock:
            self.trimmed = trimmed
            self.last_fetch = _now()
            self._arm_alarms(events, now.timestamp())
            new_sig = self._signature(events)
            old_sig = self._signature(self.events)
            self.events = events
            self.by_id = {e["id"]: e for e in events}
            if new_sig != old_sig or not self.loaded:
                self.rev += 1
                self.last_change = _now()
                log.info("events updated: %d events, rev=%d (trimmed %d)", len(events), self.rev, trimmed)
                self._save_events()
            self.loaded = True
            self.first_load = False
            self._gc_state()
            self._save_state()

    @staticmethod
    def _signature(events):
        return [(e["id"], e["summary"], e["description"], e["start"], e["allday"],
                 tuple((a["id"], a["at"]) for a in e["alarms"]),
                 e["midi_file"], e["play_duration"], e["play_repeat"]) for e in events]

    def _arm_alarms(self, events, now_ts):
        grace = int(self.cfg["alarm_grace_sec"])
        late = int(self.cfg["late_add_grace_sec"])
        for e in events:
            for a in e["alarms"]:
                st = self.alarm_state.get(a["id"])
                if st is None:
                    st = {"triggered": False, "how": "", "at": a["at"], "ts": 0,
                          "notified": False, "pushed": 0, "event": e["summary"][:60]}
                    if a["at"] < now_ts - grace:
                        # alarm already in the past when first seen
                        if self.first_load or e["start"] < now_ts - late:
                            st["triggered"] = True
                            st["how"] = "expired"
                            st["ts"] = int(now_ts)
                        else:
                            self.alog("late-add  '%s' at=%s (will ring now)", e["summary"][:40],
                                      self.fmt(a["at"]))
                    self.alarm_state[a["id"]] = st
                elif st.get("at") != a["at"]:
                    # event moved: re-arm
                    st.update({"at": a["at"], "triggered": False, "how": "", "ts": 0,
                               "notified": False, "pushed": 0})
                    self.alog("re-armed  '%s' at=%s", e["summary"][:40], self.fmt(a["at"]))
                a["triggered"] = st["triggered"]

    def _gc_state(self):
        cutoff = _now() - 14 * 86400
        self.alarm_state = {k: v for k, v in self.alarm_state.items() if v.get("at", 0) > cutoff}

    def fmt(self, ts):
        return dt.datetime.fromtimestamp(ts, self.tz).strftime("%m/%d %H:%M")

    # ------------------------------------------------------------------ tick
    def _tick_loop(self):
        while not self._stop.is_set():
            try:
                self.tick()
            except Exception as e:  # noqa: BLE001
                log.exception("tick failed: %s", e)
            time.sleep(1)

    def tick(self):
        now = _now()
        dev = self.device
        # --- device liveness ---
        with dev.lock:
            was_online = dev.online
            dev.online = (now - dev.last_seen) < int(self.cfg.device["offline_after_sec"])
            went_offline = was_online and not dev.online
        if went_offline:
            dev.anomaly(f"offline (last seen {int(now - dev.last_seen)}s ago)")
            self.alog("device OFFLINE")
            if self.cfg["ntfy_on_device_offline"]:
                self.notifier.send("M5Paper offline", "端末からのハートビートが途絶えました", tags="warning")

        # --- alarm supervision ---
        with self.lock:
            ring_timeout = int(self.cfg["device_ring_timeout_sec"])
            giveup = int(self.cfg["device_ring_giveup_sec"])
            changed = False
            for e in self.events:
                for a in e["alarms"]:
                    st = self.alarm_state.get(a["id"])
                    if st is None or st["triggered"] or a["at"] > now:
                        continue
                    overdue = now - a["at"]
                    if not st["notified"]:
                        st["notified"] = True
                        changed = True
                        off = a["offset_min"]
                        suffix = f" ({off}分前)" if off > 0 else (f" ({-off}分後)" if off < 0 else "")
                        msg = f"{self.fmt(e['start'])[6:]} {e['summary']}{suffix}"
                        self.alog("due       '%s' at=%s device=%s", e["summary"][:40], self.fmt(a["at"]),
                                  "online" if dev.online else "OFFLINE")
                        if self.cfg["ntfy_on_alarm"]:
                            self.notifier.send("M5Paper Alarm", msg)
                    if overdue > ring_timeout and dev.online and st["pushed"] < 3 \
                            and overdue > ring_timeout * (st["pushed"] + 1):
                        # device should have rung by itself; push explicitly
                        st["pushed"] += 1
                        changed = True
                        dev.enqueue(self._play_cmd(e, a), source="alarm-watchdog")
                        self.alog("push-play '%s' (no ack after %ds, push #%d)", e["summary"][:40],
                                  int(overdue), st["pushed"])
                    if overdue > giveup:
                        st["triggered"] = True
                        st["how"] = "missed-offline" if not dev.online else "missed-noack"
                        st["ts"] = int(now)
                        a["triggered"] = True
                        changed = True
                        self.rev += 1
                        self.alog("MISSED    '%s' at=%s (%s)", e["summary"][:40], self.fmt(a["at"]), st["how"])
                        self.notifier.send("M5Paper 鳴動失敗",
                                           f"{e['summary']} のアラームを端末が鳴らせませんでした ({st['how']})",
                                           tags="rotating_light")
            if changed:
                self._save_state()

    def _play_cmd(self, e, a):
        return {
            "cmd": "play",
            "alarm_id": a["id"],
            "event_id": e["id"],
            "midi": e["midi_file"] or "",
            "midi_is_url": bool(e["midi_is_url"]),
            "duration": e["play_duration"],
            "repeat": e["play_repeat"],
        }

    # ------------------------------------------------------------------ device API
    def device_heartbeat(self, body: dict, addr: str) -> dict:
        now = _now()
        dev = self.device
        dcfg = self.cfg.device
        with dev.lock:
            dev.heartbeats += 1
            if dev.first_seen == 0:
                dev.first_seen = now
            came_online = not dev.online and dev.last_seen > 0
            first_contact = dev.last_seen == 0
            dev.last_seen = now
            dev.online = True
            dev.addr = addr
            up = int(body.get("uptime", 0) or 0)
            if dev.uptime_sec and up < dev.uptime_sec - 5:
                dev.reboots += 1
                dev.anomaly(f"reboot detected (uptime {dev.uptime_sec}s -> {up}s, reason={body.get('reset')})")
                if self.cfg["ntfy_on_device_reboot"]:
                    self.notifier.send("M5Paper rebooted", f"reason={body.get('reset')}", tags="warning",
                                       priority="default")
            dev.uptime_sec = up
            seq = int(body.get("seq", -1) or -1)
            if dev.last_seq >= 0 and seq > dev.last_seq + 1 and up >= dev.uptime_sec:
                dev.seq_gaps += seq - dev.last_seq - 1
            dev.last_seq = seq
            dnow = float(body.get("now", 0) or 0)
            dev.skew_sec = (dnow - now) if dnow > 1_600_000_000 else float("nan")
            if dnow > 1_600_000_000 and abs(dev.skew_sec) > max(30, dcfg["max_skew_sec"] * 10):
                dev.anomaly(f"clock skew {dev.skew_sec:+.1f}s")
            fw = str(body.get("fw", ""))[:32]
            if dcfg.get("allowed_fw") and fw != dcfg["allowed_fw"]:
                dev.anomaly(f"unexpected firmware {fw}")
            dev.fw = fw
            dev.rev = int(body.get("rev", -1) or -1)
            dev.playing = body.get("playing")
            dev.info = {k: body.get(k) for k in (
                "fw", "uptime", "heap", "maxblock", "psram", "bat", "rssi", "ip", "ui", "rev",
                "reset", "seq", "fs_used", "fs_total", "events", "pending", "sync_fail", "temp")}
            for ev in (body.get("ev") or [])[:50]:
                if isinstance(ev, dict):
                    ev = dict(ev)
                    ev["host_ts"] = int(now)
                    dev.ui_events.append(ev)
            dev.ui_events = dev.ui_events[-500:]
        logs = body.get("log") or []
        if logs:
            self.device_log(logs[:100])
        if came_online:
            dev.anomaly("back online")
            self.alog("device ONLINE again")
        if first_contact:
            self.alog("device first contact fw=%s ip=%s", fw, body.get("ip"))

        with self.lock:
            resp = {
                "now": round(now, 3),
                "tz": self.cfg["tz_posix"],
                "rev": self.rev if self.loaded else -1,
                "hb_sec": int(dcfg["heartbeat_sec"]),
                "full_sync_sec": int(dcfg["full_sync_sec"]),
                "max_skew": dcfg["max_skew_sec"],
                "cmds": dev.pop_commands(),
                "next_alarm": self._next_alarm_ts(now),
            }
        return resp

    def _next_alarm_ts(self, now):
        best = None
        for e in self.events:
            for a in e["alarms"]:
                if not a.get("triggered") and a["at"] > now and (best is None or a["at"] < best):
                    best = a["at"]
        return best

    def device_alarm_ack(self, body: dict) -> dict:
        aid = str(body.get("alarm_id", ""))
        result = str(body.get("result", ""))[:32]
        with self.lock:
            st = self.alarm_state.get(aid)
            if st is None:
                self.device.anomaly(f"ack for unknown alarm {aid}")
                return {"ok": False, "error": "unknown alarm"}
            if not st["triggered"]:
                st["triggered"] = True
                st["how"] = f"device:{result or 'done'}"
                st["ts"] = int(_now())
                for e in self.events:
                    for a in e["alarms"]:
                        if a["id"] == aid:
                            a["triggered"] = True
                self.rev += 1
                self.alog("ACK       %s result=%s late=%ds", aid, result, int(_now() - st["at"]))
                self._save_state()
            return {"ok": True, "rev": self.rev}

    # ------------------------------------------------------------------ export for the device
    def device_header(self):
        with self.lock:
            return {
                "type": "header",
                "rev": self.rev,
                "now": round(_now(), 3),
                "tz": self.cfg["tz_posix"],
                "count": len(self.events),
                "time_24h": bool(self.cfg.device["time_24h"]),
                "text_wrap": bool(self.cfg.device["text_wrap"]),
                "play_duration": int(self.cfg["play_duration"]),
                "play_repeat": int(self.cfg["play_repeat"]),
                "alarm_offset": int(self.cfg["alarm_offset_default"]),
                "midi_default": self.cfg["midi_default"],
                "last_fetch": int(self.last_fetch),
                "src_fail": [s.idx + 1 for s in self.sources if s.ok is False],
            }

    def device_event_lines(self):
        """One compact JSON object per line (NDJSON) – parsed line-by-line on the ESP32."""
        with self.lock:
            out = []
            for e in self.events:
                out.append({
                    "id": e["id"],
                    "st": e["start"],
                    "ad": 1 if e["allday"] else 0,
                    "s": e["summary"],
                    "d": e["description"],
                    "al": [{"id": a["id"], "off": a["offset_min"], "at": a["at"],
                            "tr": 1 if a.get("triggered") else 0} for a in e["alarms"]],
                    "mf": e["midi_file"],
                    "mu": 1 if e["midi_is_url"] else 0,
                    "pd": e["play_duration"],
                    "pr": e["play_repeat"],
                })
            return out

    # ------------------------------------------------------------------ dashboard
    def status(self):
        with self.lock:
            now = _now()
            return {
                "now": now,
                "rev": self.rev,
                "events": len(self.events),
                "trimmed": self.trimmed,
                "last_fetch": self.last_fetch,
                "last_change": self.last_change,
                "sources": [{"idx": s.idx, "url": s.url, "ok": s.ok, "last_ok": s.last_ok,
                             "error": s.last_error, "fails": s.fail_count, "bytes": s.bytes}
                            for s in self.sources],
                "next_alarm": self._next_alarm_ts(now),
                "device": self.device.snapshot(),
                "window": {"past_days": self.cfg["window_past_days"],
                           "future_days": self.cfg["window_future_days"]},
                "config": {k: self.cfg[k] for k in ("ics_poll_sec", "alarm_offset_default",
                                                    "play_duration", "play_repeat", "midi_default",
                                                    "ntfy_topic", "max_events")},
            }

    def events_for_dashboard(self):
        with self.lock:
            return [dict(e, alarms=[dict(a, state=self.alarm_state.get(a["id"], {})) for a in e["alarms"]])
                    for e in self.events]
