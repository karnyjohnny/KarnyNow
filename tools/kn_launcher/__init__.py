# kn_launcher — the KarnyNow desktop launcher (round N, slice 4o).
#
# OAuth 2.0 DEVICE-grant login for GeForce NOW (docs/AUTH.md) plus a
# dark-mode PyQt5 GUI that lists the account's games (GFN GraphQL),
# assembles the knvstd.exe flags and starts a flight with its stdout
# mirrored into logs/runNNN.log.
#
# Modules:
#   store   — the credential vault (DPAPI on Windows, 0600 file elsewhere)
#   auth    — the RFC 8628 device grant + refresh + token verification
#   catalog — vpcId discovery + the games/library GraphQL pagination
#   search  — the smart title matcher ("endfield" -> "Arknights: Endfield")
#   gui     — the PyQt5 interface (QThread workers, QProcess launch)
#
# Run:  python -m kn_launcher        (from tools/, or with PYTHONPATH)
# All GFN client constants here are PUBLIC identifiers transcribed from
# docs/AUTH.md (OpenNOW provenance) — no secrets live in this package.

__version__ = "1.0.0"
