"""Server configuration (server/config.json)."""
import copy
import json
import os

DEFAULTS = {
    "listen": "0.0.0.0",
    "port": 8765,
    "api_token": "",                 # optional shared secret; device sends X-Token header
    "tz": "Asia/Tokyo",
    "tz_posix": "JST-9",             # handed to the device for localtime()

    # ---- ICS sources ----
    "ics_urls": [
        # {"url": "https://example.com/cal.ics", "user": "", "pass": ""}
    ],
    "ics_poll_sec": 60,              # host can afford 1 min (was 5-30 min on device)
    "ics_timeout_sec": 30,
    "ca_file": "",                   # extra CA bundle (e.g. certs/ca-bundle.pem) for servers with broken chains
    "ics_verify_tls": True,          # set false only as a last resort
    "window_past_days": 1,           # show yesterday too (device used today 0:00)
    "window_future_days": 60,        # device was limited to 14 days
    "max_events": 299,               # what fits in the device PSRAM table
    "max_desc_bytes": 3500,
    "max_alarms_per_event": 6,

    # ---- alarm defaults (same semantics as the old on-device config) ----
    "alarm_offset_default": 10,      # minutes before
    "play_duration": 0,              # 0 = one song
    "play_repeat": 1,
    "midi_default": "alarm.mid",
    "midi_url": "",                  # base URL for '>file.mid' markers (host downloads, device gets plain HTTP)
    "alarm_grace_sec": 600,          # at boot: alarms older than this are considered expired
    "late_add_grace_sec": 86400,     # alarms added later still ring until start+24h
    "device_ring_timeout_sec": 25,   # no ack from the device → push a play command
    "device_ring_giveup_sec": 180,   # still nothing → mark missed, notify

    # ---- notifications ----
    "ntfy_server": "https://ntfy.sh",
    "ntfy_topic": "",
    "ntfy_on_alarm": True,
    "ntfy_on_device_offline": True,
    "ntfy_on_device_reboot": True,

    # ---- device (thin client) policy ----
    "device": {
        "heartbeat_sec": 5,          # active sensing interval
        "offline_after_sec": 60,     # no heartbeat for this long → offline
        "full_sync_sec": 600,        # device re-downloads the whole table at least this often
        "time_24h": True,
        "text_wrap": False,
        "max_skew_sec": 2,           # device corrects its clock when |skew| > this
        "allowed_fw": "",            # if set, heartbeats from other versions are flagged
    },

    # ---- memory-leak monitoring (values from every heartbeat) ----
    "memory": {
        "window_sec": 21600,          # slope regression window (6 h)
        "warn_heap_kb": 80,
        "warn_maxblock_kb": 48,
        "warn_slope_kb_per_h": 2,     # sustained decline faster than this → warning
        "reboot_heap_kb": 60,         # below this → preventive reboot (when safe)
        "reboot_maxblock_kb": 36,
        "reboot_guard_sec": 900,      # never reboot within 15 min of an alarm
        "ntfy": True,
    },

    "data_dir": "data",
    "cache_dir": "cache",
}


def _merge(base, override):
    out = copy.deepcopy(base)
    for k, v in (override or {}).items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = _merge(out[k], v)
        else:
            out[k] = v
    return out


# Directory holding the old SD-card contents (config.json, fonts/, midi/).
# /etc/m5sched is preferred; /etc/m5shed is accepted as an alias.
SYSTEM_DIRS = ["/etc/m5sched", "/etc/m5shed"]


def find_system_dir():
    for d in SYSTEM_DIRS:
        if os.path.isdir(d):
            return d
    return None


def default_config_path(fallback):
    """/etc/m5sched/config.json wins over server/config.json when it exists."""
    d = find_system_dir()
    if d and os.path.isfile(os.path.join(d, "config.json")):
        return os.path.join(d, "config.json")
    return fallback


class Config:
    def __init__(self, path):
        self.path = os.path.abspath(path)
        self.base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # server/
        self.system_dir = find_system_dir()
        self.reload()

    def reload(self):
        raw = {}
        if os.path.exists(self.path):
            with open(self.path, "r", encoding="utf-8") as f:
                raw = json.load(f)
        # keys from the old on-device config that encoded ESP32 memory/SSL compromises — ignored
        for k in ("max_events", "max_desc_bytes", "min_free_heap", "ics_poll_min", "sd_log_enabled"):
            raw.pop(k, None)
        self.d = _merge(DEFAULTS, raw)
        self._apply_legacy(raw)
        self.data_dir = self._abs(self.d["data_dir"])
        self.cache_dir = self._abs(self.d["cache_dir"])
        for sub in ("", "log", "screenshots"):
            os.makedirs(os.path.join(self.data_dir, sub), exist_ok=True)
        os.makedirs(os.path.join(self.cache_dir, "midi"), exist_ok=True)
        os.makedirs(os.path.join(self.cache_dir, "ics"), exist_ok=True)

    def _apply_legacy(self, raw):
        """Accept the old on-device SD config.json keys as-is."""
        d = self.d
        if isinstance(raw.get("ics_url"), str) and not raw.get("ics_urls"):
            d["ics_urls"] = [
                {"url": u.strip(), "user": raw.get("ics_user", ""), "pass": raw.get("ics_pass", "")}
                for u in raw["ics_url"].split(",") if u.strip()
            ]
        if "alarm_offset" in raw:
            d["alarm_offset_default"] = int(raw["alarm_offset"])
        if isinstance(raw.get("midi_file"), str) and "midi_default" not in raw:
            d["midi_default"] = os.path.basename(raw["midi_file"]) or d["midi_default"]
        for k in ("time_24h", "text_wrap"):
            if k in raw and k not in (raw.get("device") or {}):
                d["device"][k] = bool(raw[k])
        # legacy max_events / max_desc_bytes / min_free_heap were ESP32 memory compromises;
        # deliberately ignored — the host-side defaults (299 / 3500) apply unless set explicitly
        # via the new keys.
        # server-side paths live next to the package unless given absolute
        if self.system_dir:
            d.setdefault("midi_dir", os.path.join(self.system_dir, "midi"))

    def _abs(self, p):
        return p if os.path.isabs(p) else os.path.join(self.base_dir, p)

    def __getitem__(self, k):
        return self.d[k]

    def get(self, k, default=None):
        return self.d.get(k, default)

    @property
    def device(self):
        return self.d["device"]
