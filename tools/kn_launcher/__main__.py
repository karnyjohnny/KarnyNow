# __main__.py — entry: python -m kn_launcher
from __future__ import annotations

import sys


def main() -> int:
    from . import gui
    return gui.run()


if __name__ == "__main__":
    sys.exit(main())
