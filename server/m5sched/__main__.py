"""python -m m5sched [config.json]"""
import logging
import os
import signal
import sys
import threading

from .api import serve
from .config import Config
from .core import Scheduler
from .notify import Notifier


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "config.json")
    logging.basicConfig(level=os.environ.get("M5SCHED_LOG", "INFO"),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = Config(path)
    if not cfg["ics_urls"]:
        logging.warning("no ics_urls configured in %s", cfg.path)
    notifier = Notifier(cfg)
    sched = Scheduler(cfg, notifier)
    sched.start()
    httpd = serve(sched, cfg)

    stop_flag = threading.Event()

    def _stop(*_):
        stop_flag.set()

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    # serve in a worker thread so the main thread can handle signals and exit promptly
    t = threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.5}, daemon=True)
    t.start()
    stop_flag.wait()
    logging.info("shutting down")
    sched.stop()
    httpd.shutdown()
    httpd.server_close()


if __name__ == "__main__":
    main()
