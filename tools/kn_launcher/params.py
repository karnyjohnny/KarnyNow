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
                  "--session-release --session-interactive "
                  "--wire-dump=logs\\wire_r1.txt")


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
