# params.py — the operator's knvstd parameter block: default text and
# the Windows argv splitter.
#
# F37 fix (round N slice 4o): gui.py used shlex.split(text) — POSIX
# mode, which treats the backslash as an escape character. On Windows
# the operator's default "--wire-dump=logs\wire_r1.txt" therefore
# arrived at knvstd as "--wire-dump=logswire_r1.txt" (silent path
# corruption on EVERY launch). shlex posix=False is not the answer
# either (it keeps mid-token quotes verbatim). split_params below
# implements the CommandLineToArgvW/MSVCRT rules knvstd's own C runtime
# uses, so what the operator types is what knvstd's argv sees:
#   * whitespace separates entries unless inside "quotes";
#   * a backslash is LITERAL, except directly before a quote;
#   * 2n backslashes + " -> n backslashes, quote toggles grouping;
#   * 2n+1 backslashes + " -> n backslashes + a literal quote.
# QProcess then re-quotes each argv entry for the child's command line
# using the same rules — the round trip is exact.
#
# Python 3.8 (the Win7 rig, tests/requirements.txt): the future import
# below makes every annotation lazy — PEP 604/585 spellings in
# signatures would otherwise raise TypeError AT IMPORT TIME on 3.8.

from __future__ import annotations

DEFAULT_PARAMS = ("--session-seconds=1200 --input-anti-afk=60000 "
                  "--session-present --session-audio-play "
                  "--session-release --session-interactive")

# Slice 4q: --wire-dump is NO LONGER in the default block. The launcher
# owns it now — every draw gets its own folder and the dump goes inside
# it, so two draws of one run can never overwrite each other's
# wire_r1.txt (they used to: logs\wire_r1.txt was a single fixed path).
WIRE_DUMP_NAME = "wire_r1.txt"
CONSOLE_LOG_NAME = "console.log"


def flag_value(argv, name):
    # type: (list, str) -> str
    """The value of --name=VALUE in argv, or None when absent. `name`
    is given without the leading dashes."""
    prefix = "--%s=" % name
    for a in argv or []:
        if a.startswith(prefix):
            return a[len(prefix):]
    return None


def has_flag(argv, name):
    # type: (list, str) -> bool
    """True when --name or --name=... is present (boolean flags too)."""
    bare = "--%s" % name
    for a in argv or []:
        if a == bare or a.startswith(bare + "="):
            return True
    return False


def drop_flags(argv, names):
    # type: (list, list) -> list
    """argv minus every occurrence of the named flags (with or without
    a value). Used so an operator-typed --log-dir/--wire-dump cannot
    fight the launcher's per-draw folder."""
    out = []
    for a in argv or []:
        hit = False
        for n in names:
            if a == "--%s" % n or a.startswith("--%s=" % n):
                hit = True
                break
        if not hit:
            out.append(a)
    return out


def draw_flags(draw_dir):
    # type: (str) -> list
    """The two flags that make a draw self-contained inside its folder:
    knvstd's own per-channel logs (--log-dir, which also receives the
    diag bundle) and the TLS plaintext mirror (--wire-dump)."""
    import os as _os
    return ["--log-dir=%s" % draw_dir,
            "--wire-dump=%s" % _os.path.join(draw_dir, WIRE_DUMP_NAME)]


def inject_draw_flags(argv, draw_dir):
    # type: (list, str) -> list
    """argv + the per-draw log flags. An operator-typed --log-dir or
    --wire-dump is REPLACED (not duplicated) — the run folder is the
    contract, so the flags always point inside it."""
    out = drop_flags(argv, ["log-dir", "wire-dump"])
    return out + draw_flags(draw_dir)


def redact_argv(argv, token=""):
    # type: (list, str) -> list
    """argv safe to print: the session token becomes a length marker.
    The token reaches knvstd through argv only (never the parameter box,
    never a log file) — this is the one place it is displayed."""
    out = []
    for a in argv or []:
        if a.startswith("--session-token="):
            v = a[len("--session-token="):]
            out.append("--session-token=<redacted:%d>" % (len(token) if token
                                                          else len(v)))
        else:
            out.append(a)
    return out


def split_params(text):
    # type: (str) -> list
    """argv list from the parameter box — Windows paths stay intact."""
    argv = []
    cur = []
    in_token = False
    in_quotes = False
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            in_quotes = not in_quotes
            in_token = True
            i += 1
            continue
        if c == "\\":
            # count the backslash run
            j = i
            while j < n and text[j] == "\\":
                j += 1
            backs = j - i
            if j < n and text[j] == '"':
                cur.append("\\" * (backs // 2))
                if backs % 2:                 # escaped -> literal quote
                    cur.append('"')
                    i = j + 1
                else:                         # quote toggles grouping
                    in_quotes = not in_quotes
                    i = j + 1
                in_token = True
                continue
            cur.append("\\" * backs)          # literal run
            in_token = True
            i = j
            continue
        if c.isspace() and not in_quotes:
            if in_token:
                argv.append("".join(cur))
                cur = []
                in_token = False
            i += 1
            continue
        cur.append(c)
        in_token = True
        i += 1
    if in_token:
        argv.append("".join(cur))
    return argv
