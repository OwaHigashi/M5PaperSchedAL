"""ICS fetch + parse + '!' alarm-marker grammar (ported from ics_parser.cpp).

Everything that the ESP32 struggled with lives here: TLS, big buffers,
time zones, full RRULE/RECURRENCE-ID/EXDATE handling (recurring_ical_events).
"""
import datetime as dt
import hashlib
import logging
import os
import re
import unicodedata
from zoneinfo import ZoneInfo

import requests
from icalendar import Calendar
import recurring_ical_events

log = logging.getLogger("ics")

# ----------------------------------------------------------------------------
# Alarm marker grammar  (identical semantics to parseAlarmMarker() in C++)
#   "!"            lone bang       -> default offset
#   "!-10!"        minutes before  "!+5!" minutes after   "!-25,-15,-5!" several
#   ">file.mid"    MIDI from URL   "<file.mid" MIDI from device storage
#   "@N"           ring N seconds (0 = one song)   "*N" repeat N times
# ----------------------------------------------------------------------------

def _normalize_fullwidth(s: str) -> str:
    """Full-width ASCII punctuation/digits -> ASCII (NFKC is close to the C++ table)."""
    out = []
    for ch in s:
        o = ord(ch)
        if 0xFF01 <= o <= 0xFF5E:
            out.append(chr(o - 0xFEE0))
        elif ch == "　":
            out.append(" ")
        else:
            out.append(ch)
    return "".join(out)


_NUM_RE = re.compile(r"[0-9 ]+")


def parse_alarm_marker(text: str, default_offset: int, max_offsets: int):
    """Return dict(found, offsets[], midi_file, midi_is_url, duration, repeat) or None."""
    if not text:
        return None
    s = _normalize_fullwidth(text)
    if "!" not in s:
        return None
    offsets = []
    midi_file = ""
    midi_is_url = False
    duration = -1
    repeat = -1
    found = False

    def push(v):
        if len(offsets) < max_offsets and v not in offsets:
            offsets.append(v)

    pos = 0
    while pos < len(s):
        p = s.find("!", pos)
        if p < 0:
            break
        e = s.find("!", p + 1)
        if e < 0:
            # lone '!' -> alarm with default offset
            found = True
            if not offsets:
                push(default_offset)
            break
        found = True
        content = s[p + 1:e]
        block_has_offset = False
        this_file = ""
        this_is_url = False
        i = 0
        n = len(content)
        while i < n:
            c = content[i]
            if c in "-+":
                m = _NUM_RE.match(content, i + 1)
                if m and m.group().strip():
                    val = int(m.group().replace(" ", "") or "0")
                    if 0 <= val <= 24 * 60:
                        push(val if c == "-" else -val)
                        block_has_offset = True
                    i = m.end()
                else:
                    i += 1
            elif c in ", ":
                i += 1
            elif c in "<>":
                this_is_url = (c == ">")
                j = i + 1
                while j < n and content[j] not in "-+@*<>":
                    j += 1
                if j > i + 1:
                    this_file = content[i + 1:j].strip()
                i = j
            elif c == "@":
                m = _NUM_RE.match(content, i + 1)
                if m and m.group().strip():
                    duration = int(m.group().replace(" ", ""))
                    i = m.end()
                else:
                    duration = 0
                    i += 1
            elif c == "*":
                m = _NUM_RE.match(content, i + 1)
                if m and m.group().strip():
                    repeat = int(m.group().replace(" ", ""))
                    i = m.end()
                else:
                    i += 1
            else:
                i += 1
        if not block_has_offset:
            push(default_offset)
        if this_file:
            midi_file = this_file
            midi_is_url = this_is_url
        pos = e + 1

    if not found:
        return None
    if not offsets:
        push(default_offset)
    return dict(offsets=offsets, midi_file=midi_file, midi_is_url=midi_is_url,
                duration=duration, repeat=repeat)


# ----------------------------------------------------------------------------
# Fetch with on-disk fallback (last good copy survives server restarts / outages)
# ----------------------------------------------------------------------------

class IcsSource:
    def __init__(self, idx, spec, cache_dir, timeout, verify=True):
        self.idx = idx
        self.verify = spec.get("verify", verify)
        self.url = spec["url"]
        self.user = spec.get("user") or ""
        self.password = spec.get("pass") or ""
        self.timeout = timeout
        self.cache_path = os.path.join(cache_dir, "ics", f"src{idx}.ics")
        self.ok = None            # None=never tried, True/False
        self.last_ok = 0.0
        self.last_error = ""
        self.fail_count = 0
        self.bytes = 0

    def fetch(self) -> bytes | None:
        """Return the ICS body (fresh or cached). None if nothing at all."""
        try:
            if self.url.startswith("file://"):           # local file (testing / exported calendars)
                with open(self.url[7:], "rb") as f:
                    body = f.read()
                self.ok = True; self.last_ok = dt.datetime.now().timestamp(); self.last_error = ""
                self.fail_count = 0; self.bytes = len(body)
                return body
            auth = (self.user, self.password) if self.user else None
            r = requests.get(self.url, auth=auth, timeout=self.timeout, verify=self.verify,
                             headers={"Cache-Control": "no-cache", "User-Agent": "m5sched/1.0"})
            r.raise_for_status()
            body = r.content
            if b"BEGIN:VCALENDAR" not in body[:4096]:
                raise ValueError("response is not an ICS file")
            tmp = self.cache_path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(body)
            os.replace(tmp, self.cache_path)
            self.ok = True
            self.last_ok = dt.datetime.now().timestamp()
            self.last_error = ""
            self.fail_count = 0
            self.bytes = len(body)
            return body
        except Exception as e:  # noqa: BLE001
            self.ok = False
            self.fail_count += 1
            self.last_error = f"{type(e).__name__}: {e}"
            log.warning("ICS[%d] fetch failed (%d): %s", self.idx, self.fail_count, self.last_error)
            if os.path.exists(self.cache_path):
                with open(self.cache_path, "rb") as f:
                    return f.read()
            return None


# ----------------------------------------------------------------------------
# Parse + expand into the window
# ----------------------------------------------------------------------------

def _to_aware(v, tz):
    """icalendar date/datetime -> (aware datetime, is_allday)."""
    if isinstance(v, dt.datetime):
        if v.tzinfo is None:
            return v.replace(tzinfo=tz), False
        return v.astimezone(tz), False
    if isinstance(v, dt.date):
        return dt.datetime(v.year, v.month, v.day, tzinfo=tz), True
    raise TypeError(type(v))


def _clean_text(s) -> str:
    if s is None:
        return ""
    s = str(s)
    s = s.replace("\r\n", "\n").replace("\r", "\n")
    # the device renderer understands the literal 2-char sequence "\n" (ICS style)
    s = s.replace("\\n", "\n")
    s = s.replace("\n", "\\n")
    return s.strip()


def _stable_id(uid: str, start: dt.datetime) -> str:
    h = hashlib.sha1(f"{uid}|{int(start.timestamp())}".encode("utf-8")).hexdigest()
    return h[:12]


def parse_sources(bodies, cfg, now=None):
    """bodies: list of (src_idx, bytes).  Returns list of event dicts sorted by start."""
    tz = ZoneInfo(cfg["tz"])
    now = now or dt.datetime.now(tz)
    today0 = now.replace(hour=0, minute=0, second=0, microsecond=0)
    win_start = today0 - dt.timedelta(days=int(cfg["window_past_days"]))
    win_end = today0 + dt.timedelta(days=int(cfg["window_future_days"]) + 1)
    default_offset = int(cfg["alarm_offset_default"])
    max_alarms = int(cfg["max_alarms_per_event"])
    max_desc = int(cfg["max_desc_bytes"])

    events = []
    seen = set()
    for src_idx, body in bodies:
        try:
            cal = Calendar.from_ical(body)
        except Exception as e:  # noqa: BLE001
            log.error("ICS[%d] parse failed: %s", src_idx, e)
            continue
        try:
            occurrences = recurring_ical_events.of(cal).between(win_start, win_end)
        except Exception as e:  # noqa: BLE001
            log.error("ICS[%d] recurrence expansion failed: %s", src_idx, e)
            continue
        for ve in occurrences:
            if ve.name != "VEVENT":
                continue
            try:
                dtstart = ve.get("DTSTART")
                if dtstart is None:
                    continue
                start, allday = _to_aware(dtstart.dt, tz)
                end = None
                if ve.get("DTEND") is not None:
                    end, _ = _to_aware(ve.get("DTEND").dt, tz)
                status = str(ve.get("STATUS", "")).upper()
                if status == "CANCELLED":
                    continue
                uid = str(ve.get("UID", "")) or f"noid-{src_idx}"
                summary = _clean_text(ve.get("SUMMARY"))
                desc = _clean_text(ve.get("DESCRIPTION"))
                loc = _clean_text(ve.get("LOCATION"))
                eid = _stable_id(uid, start)
                if eid in seen:
                    continue
                seen.add(eid)

                marker = parse_alarm_marker(summary, default_offset, max_alarms)
                if marker is None and desc:
                    marker = parse_alarm_marker(desc, default_offset, max_alarms)

                if len(desc.encode("utf-8")) > max_desc:
                    b = desc.encode("utf-8")[:max_desc]
                    desc = b.decode("utf-8", errors="ignore")

                ev = {
                    "id": eid,
                    "uid": uid,
                    "src": src_idx,
                    "start": int(start.timestamp()),
                    "end": int(end.timestamp()) if end else None,
                    "allday": allday,
                    "summary": summary,
                    "description": desc,
                    "location": loc,
                    "has_alarm": marker is not None,
                    "midi_file": marker["midi_file"] if marker else "",
                    "midi_is_url": marker["midi_is_url"] if marker else False,
                    "play_duration": marker["duration"] if marker else -1,
                    "play_repeat": marker["repeat"] if marker else -1,
                    "alarms": [],
                }
                if marker:
                    for off in marker["offsets"]:
                        at = int(start.timestamp()) - off * 60
                        ev["alarms"].append({
                            "id": f"{eid}-{off:+d}",
                            "offset_min": off,
                            "at": at,
                        })
                events.append(ev)
            except Exception as e:  # noqa: BLE001
                log.exception("ICS[%d] event skipped: %s", src_idx, e)

    events.sort(key=lambda e: (e["start"], e["summary"]))
    return events


def trim_events(events, max_events, now_ts):
    """Keep at most max_events, preferring today/future (same policy as the device)."""
    if len(events) <= max_events:
        return events, 0
    today_idx = len(events)
    for i, e in enumerate(events):
        if e["start"] >= now_ts - 86400:
            today_idx = i
            break
    future = len(events) - today_idx
    keep_past = max(0, min(today_idx, 10, max_events - future))
    start = today_idx - keep_past
    out = events[start:start + max_events]
    return out, len(events) - len(out)


def normalize_nfc(s):
    return unicodedata.normalize("NFC", s)
