"""Memory-leak monitor for the M5Paper.

Every heartbeat carries heap / maxblock (largest free DRAM block) / psram.  The
host keeps the series, writes one CSV line per minute (data/log/mem.csv), fits
a slope over a sliding window, and raises warnings.  It can also ask the device
to reboot *preventively* — only when nothing is playing and no alarm is due
soon — so a slow leak never turns into a missed alarm.
"""
import datetime as dt
import logging
import os
import time
from collections import deque

log = logging.getLogger("memmon")


class MemMonitor:
    def __init__(self, cfg, data_dir):
        m = cfg.get("memory") or {}
        self.window_sec = int(m.get("window_sec", 6 * 3600))        # regression window
        self.warn_heap = int(m.get("warn_heap_kb", 80)) * 1024
        self.warn_maxblock = int(m.get("warn_maxblock_kb", 48)) * 1024
        self.warn_slope = -abs(int(m.get("warn_slope_kb_per_h", 2))) * 1024   # bytes/hour
        self.reboot_heap = int(m.get("reboot_heap_kb", 60)) * 1024
        self.reboot_maxblock = int(m.get("reboot_maxblock_kb", 36)) * 1024
        self.reboot_guard_sec = int(m.get("reboot_guard_sec", 900))  # no reboot within this of an alarm
        self.csv_path = os.path.join(data_dir, "log", "mem.csv")
        self.samples = deque(maxlen=24 * 3600 // 5)   # ~24h of 5s samples
        self.last_csv = 0.0
        self.boot_baseline = None      # (ts, heap, maxblock) taken ~5 min after boot
        self.boot_ts = 0.0
        self.warned = set()            # warnings already sent this boot
        self.last_reboot_req = 0.0
        self.min_heap = None
        self.min_maxblock = None

    # ---- ingest -------------------------------------------------------
    def add(self, ts, uptime, heap, maxblock, psram, rebooted=False):
        if heap is None or maxblock is None:
            return
        if rebooted or self.boot_ts == 0:
            self.boot_ts = ts - uptime
            self.boot_baseline = None
            self.warned.clear()
            self.min_heap = self.min_maxblock = None
        self.samples.append((ts, int(heap), int(maxblock), int(psram or 0)))
        self.min_heap = heap if self.min_heap is None else min(self.min_heap, heap)
        self.min_maxblock = maxblock if self.min_maxblock is None else min(self.min_maxblock, maxblock)
        if self.boot_baseline is None and uptime >= 300:
            self.boot_baseline = (ts, int(heap), int(maxblock))
        if ts - self.last_csv >= 60:
            self.last_csv = ts
            try:
                new = not os.path.exists(self.csv_path)
                with open(self.csv_path, "a", encoding="utf-8") as f:
                    if new:
                        f.write("time,uptime_s,heap,maxblock,psram\n")
                    f.write(f"{dt.datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')},{int(uptime)},{heap},{maxblock},{psram}\n")
            except OSError as e:
                log.warning("mem.csv write failed: %s", e)

    # ---- analysis -----------------------------------------------------
    def slope(self, field=1, window=None):
        """Least-squares slope in bytes/hour over the window (None if < 10 min of data)."""
        window = window or self.window_sec
        if len(self.samples) < 2:
            return None
        t1 = self.samples[-1][0]
        pts = [(s[0], s[field]) for s in self.samples if t1 - s[0] <= window]
        if len(pts) < 12 or pts[-1][0] - pts[0][0] < 600:
            return None
        n = len(pts)
        mt = sum(p[0] for p in pts) / n
        my = sum(p[1] for p in pts) / n
        sxx = sum((p[0] - mt) ** 2 for p in pts)
        if sxx == 0:
            return None
        sxy = sum((p[0] - mt) * (p[1] - my) for p in pts)
        return sxy / sxx * 3600.0

    def assess(self):
        """Return dict(level, reasons[], stats)."""
        if not self.samples:
            return {"level": "unknown", "reasons": [], "stats": {}}
        ts, heap, maxblock, psram = self.samples[-1]
        sl_h = self.slope(1)
        sl_m = self.slope(2)
        reasons = []
        level = "ok"
        if heap < self.warn_heap:
            reasons.append(f"heap {heap // 1024}KB < {self.warn_heap // 1024}KB")
            level = "warn"
        if maxblock < self.warn_maxblock:
            reasons.append(f"maxBlock {maxblock // 1024}KB < {self.warn_maxblock // 1024}KB")
            level = "warn"
        if sl_h is not None and sl_h < self.warn_slope:
            reasons.append(f"heap falling {sl_h / 1024:.1f}KB/h")
            level = "warn"
        if sl_m is not None and sl_m < self.warn_slope:
            reasons.append(f"maxBlock falling {sl_m / 1024:.1f}KB/h")
            level = "warn"
        if heap < self.reboot_heap or maxblock < self.reboot_maxblock:
            level = "critical"
        since_boot = None
        if self.boot_baseline:
            since_boot = {"heap": heap - self.boot_baseline[1], "maxblock": maxblock - self.boot_baseline[2],
                          "hours": round((ts - self.boot_baseline[0]) / 3600, 2)}
        hours_to_floor = None
        if sl_h is not None and sl_h < 0:
            hours_to_floor = round((heap - self.reboot_heap) / (-sl_h), 1)
        return {
            "level": level,
            "reasons": reasons,
            "stats": {
                "heap": heap, "maxblock": maxblock, "psram": psram,
                "min_heap": self.min_heap, "min_maxblock": self.min_maxblock,
                "slope_heap_kb_h": None if sl_h is None else round(sl_h / 1024, 2),
                "slope_maxblock_kb_h": None if sl_m is None else round(sl_m / 1024, 2),
                "since_boot": since_boot,
                "hours_to_reboot_floor": hours_to_floor,
                "samples": len(self.samples),
                "window_h": self.window_sec / 3600,
                "uptime_h": round((ts - self.boot_ts) / 3600, 2) if self.boot_ts else None,
            },
        }

    def series(self, step_sec=300):
        """Down-sampled series for the dashboard chart."""
        out = []
        last = 0
        for s in self.samples:
            if s[0] - last >= step_sec:
                out.append([int(s[0]), s[1], s[2]])
                last = s[0]
        if self.samples and (not out or out[-1][0] != int(self.samples[-1][0])):
            s = self.samples[-1]
            out.append([int(s[0]), s[1], s[2]])
        return out

    # ---- decisions ----------------------------------------------------
    def should_reboot(self, now, playing, next_alarm_ts):
        """Preventive reboot: critical memory, nothing playing, no alarm within the guard window."""
        a = self.assess()
        if a["level"] != "critical":
            return False, ""
        if playing:
            return False, "playing"
        if next_alarm_ts and 0 < next_alarm_ts - now < self.reboot_guard_sec:
            return False, "alarm soon"
        if now - self.last_reboot_req < 600:
            return False, "recently requested"
        self.last_reboot_req = now
        return True, "; ".join(a["reasons"]) or "critical memory"

    def new_warning(self, key):
        if key in self.warned:
            return False
        self.warned.add(key)
        return True
