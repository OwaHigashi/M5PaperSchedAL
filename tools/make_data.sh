#!/bin/bash
# Build the LittleFS image source dir (data/) from the old SD-card layout in /etc/m5sched
# (config.json, fonts/ipaexg.ttf, midi/*.mid) and flash it.   Usage: tools/make_data.sh [--no-flash]
set -e
cd "$(dirname "$0")/.."
SYS=""; for d in /etc/m5sched; do [ -d "$d" ] && { SYS=$d; break; }; done
[ -n "$SYS" ] || { echo "no /etc/m5sched directory"; exit 1; }
mkdir -p data/fonts data/midi
[ -f "$SYS/fonts/ipaexg.ttf" ] && cp "$SYS/fonts/ipaexg.ttf" data/fonts/
[ -d "$SYS/midi" ] && cp "$SYS"/midi/*.mid data/midi/ 2>/dev/null || true
# device config: wifi + midi settings come from the old config.json; server = this host
python3 - "$SYS/config.json" <<'PY'
import json, sys, socket, os
old = json.load(open(sys.argv[1])) if os.path.exists(sys.argv[1]) else {}
cur = json.load(open("data/config.json")) if os.path.exists("data/config.json") else {}
def host_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(("10.255.255.255", 1)); ip = s.getsockname()[0]; s.close(); return ip
cfg = {
  "wifi_ssid": old.get("wifi_ssid", cur.get("wifi_ssid", "")),
  "wifi_pass": old.get("wifi_pass", cur.get("wifi_pass", "")),
  "server_host": old.get("server_host", cur.get("server_host", host_ip())),
  "server_port": old.get("server_port", cur.get("server_port", 8765)),
  "api_token": old.get("api_token", cur.get("api_token", "")),
  "midi_file": "/midi/" + os.path.basename(old.get("midi_file", "/midi/alarm.mid")),
  "midi_baud": old.get("midi_baud", 31250),
  "port_select": old.get("port_select", 1),
}
json.dump(cfg, open("data/config.json", "w"), indent=2, ensure_ascii=False)
print("data/config.json:", {k: ("***" if k == "wifi_pass" else v) for k, v in cfg.items()})
PY
ls -la data data/fonts data/midi
[ "$1" = "--no-flash" ] || pio run -t uploadfs
