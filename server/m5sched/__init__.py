"""M5Paper schedule host server.

The Ubuntu host does everything that needs TLS, memory or time-keeping
(ICS fetch, RRULE expansion, alarm state, ntfy, MIDI download) and exposes a
plain-HTTP REST API on the LAN.  The M5Paper is a thin "display + ring" client
that is never trusted: it only reports what it sees and acks what it did.
"""
__version__ = "1.0.0"
