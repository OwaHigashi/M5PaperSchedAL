"""ntfy push notifications (done on the host; the device never talks TLS)."""
import logging
import threading

import requests

log = logging.getLogger("ntfy")


class Notifier:
    def __init__(self, cfg):
        self.cfg = cfg

    def send(self, title, message, priority="high", tags="alarm_clock"):
        topic = self.cfg.get("ntfy_topic") or ""
        if not topic:
            log.info("ntfy disabled; would send: %s / %s", title, message)
            return
        url = f"{self.cfg['ntfy_server'].rstrip('/')}/{topic}"

        def _do():
            try:
                r = requests.post(url, data=message.encode("utf-8"), timeout=15,
                                  headers={"Title": title.encode("utf-8").decode("latin-1", "replace")
                                           if not title.isascii() else title,
                                           "Priority": priority, "Tags": tags})
                log.info("ntfy %s -> HTTP %d", title, r.status_code)
            except Exception as e:  # noqa: BLE001
                log.warning("ntfy failed: %s", e)

        threading.Thread(target=_do, daemon=True, name="ntfy").start()
