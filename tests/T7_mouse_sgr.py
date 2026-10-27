# -*- coding: utf-8 -*-
"""
T7 - Czy mysz dziala przez ANSI/SGR w ConEmu? (klikalna lista gier)

Po co: T3 potwierdzil, ze ReadConsoleInputW daje zdarzenia myszy (146 w 20 s), ale
T5 pokazal looks_like_sgr=false w OBU hostach. T5 byl jednak niemiarodajny: czytal
stdin przez msvcrt.getwch(), ktory na Windows nie skleja poprawnie sekwencji
wielobajtowych. Ten test czyta SUROWE BAJTY z fd 0 i sam je skleja.

Rozstrzyga trzy rzeczy:
  1. Czy host (ConEmu) w ogole wysyla raporty myszy po wlaczeniu trybu 1000/1002/1006
  2. W jakim formacie: SGR (ESC[<b;x;yM/m) czy X10 (ESC[M + 3 bajty)
  3. Czy dzialaja zdarzenia resize (ESC[8;H;Wt) - potrzebne do przerysowania TUI

Od tego zalezy projekt TUI:
  - jesli SGR dziala  -> Textual dostaje mysz i lista gier jest klikalna
  - jesli nie dziala  -> nawigacja wylacznie klawiatura (T3 potwierdzil komplet VK:
                         strzalki 37-40, Enter 13, F1 112, litery) - to tez jest OK,
                         ale trzeba to wiedziec ZANIM narysujemy UI

Uruchomienie (W CONEMU, nie w czystym cmd - patrz decyzja D1):
    python T7_mouse_sgr.py

Przez 15 sekund: poruszaj mysza nad oknem konsoli, kliknij LPM 2 razy, PPM 1 raz,
krolkiem przewin, i zmien raz rozmiar okna (przeciągnij krawędź ConEmu).

Wynik: T7_wynik.json
"""

import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "T7_wynik.json")

R = {"test": "T7_mouse_sgr",
     "when_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
     "python": sys.version}

SEQ_RE = re.compile(r"\x1b\[<(\d+);(\d+);(\d+)([Mm])")     # SGR mouse
X10_RE = re.compile(r"\x1b\[M")                            # X10 mouse
RESIZE_RE = re.compile(r"\x1b\[8;(\d+);(\d+)t")            # resize
KEY_RE = re.compile(r"\x1b\[([0-9;]*)([A-Za-z~])")         # klawisze CSI


def out(m):
    try:
        sys.stdout.write(m + "\n")
    except UnicodeEncodeError:
        sys.stdout.write((m + "\n").encode("ascii", "replace").decode("ascii"))
    sys.stdout.flush()


def write_fd0_flush(s):
    sys.stdout.write(s)
    sys.stdout.flush()


def main():
    if os.name != "nt":
        R["fatal"] = "not windows"
        _save()
        return 2

    # --- kontekst hosta ---
    R["host"] = {
        "conemuansi": os.environ.get("CONEMUANSI"),
        "conemuhooks": os.environ.get("CONEMUHOOKS"),
        "conemu_build": os.environ.get("CONEMUBUILD"),
        "ansicon": os.environ.get("ANSICON"),
        "term": os.environ.get("TERM"),
        "colorterm": os.environ.get("COLORTERM"),
    }
    try:
        import ctypes
        import ctypes.wintypes as wt
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi")
        psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                               wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
        psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
        psapi.GetModuleFileNameExW.restype = wt.DWORD
        k32.GetCurrentProcess.restype = wt.HANDLE
        h = k32.GetCurrentProcess()
        arr = (ctypes.c_void_p * 1024)()
        need = wt.DWORD(0)
        mods = []
        if psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x03):
            cnt = min(1024, need.value // ctypes.sizeof(ctypes.c_void_p))
            nb = ctypes.create_unicode_buffer(1024)
            for i in range(cnt):
                if psapi.GetModuleFileNameExW(h, arr[i], nb, 1024):
                    mods.append(nb.value)
        R["host"]["hook_dll_loaded"] = [m for m in mods if "conemuhk" in m.lower()]
        R["host"]["modules_count"] = len(mods)
    except Exception as exc:
        R["host"]["module_error"] = repr(exc)

    is_conemu = bool(R["host"].get("conemuansi") or R["host"].get("hook_dll_loaded"))
    R["host"]["looks_like_conemu"] = is_conemu
    out("=== Host ===")
    out("  ConEmu wykryty: %s (CONEMUANSI=%s, hook=%s)"
        % (is_conemu, R["host"].get("conemuansi"), R["host"].get("hook_dll_loaded")))
    if not is_conemu:
        out("  OSTRZEZENIE: to nie wyglada na ConEmu. Wynik bedzie niemiarodajny")
        out("  (w czystym cmd.exe ANSI w ogole nie dziala - patrz T5_wynik_2).")

    # --- tryb surowy stdin ---
    import msvcrt
    try:
        fd = msvcrt.get_osfhandle(0)
        R["stdin_osfhandle"] = fd
    except Exception as exc:
        R["stdin_osfhandle_error"] = repr(exc)

    # wylacz tryby liniowe zeby ESC dochodzil natychmiast
    try:
        import ctypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        hin = k32.GetStdHandle(-10)
        mode = ctypes.wintypes.DWORD(0)
        k32.GetConsoleMode(hin, ctypes.byref(mode))
        R["input_mode_before"] = hex(mode.value)
        # zostaw ENABLE_WINDOW_INPUT i ENABLE_MOUSE_INPUT, wylacz liniowe/echo
        newmode = (mode.value | 0x0008 | 0x0010 | 0x0080) & ~0x0002 & ~0x0004 & ~0x0040
        R["set_input_mode_ok"] = bool(k32.SetConsoleMode(hin, newmode))
        R["input_mode_during"] = hex(newmode)
    except Exception as exc:
        R["console_mode_error"] = repr(exc)

    # --- wlacz raportowanie myszy ---
    write_fd0_flush("\x1b[?1000h")   # podstawowe zdarzenia przyciskow
    write_fd0_flush("\x1b[?1002h")   # + ruch z przyciskiem (button-motion)
    write_fd0_flush("\x1b[?1003h")   # + kazdy ruch (any-motion)
    write_fd0_flush("\x1b[?1006h")   # format SGR
    write_fd0_flush("\x1b[?1015h")   # format urxvt (alternatywny, do porownania)
    R["modes_enabled"] = [1000, 1002, 1003, 1006, 1015]
    out("=== Wlaczylem raportowanie myszy (1000/1002/1003 + SGR 1006 + urxvt 1015) ===")
    out("  Przez 15 sekund:")
    out("   - poruszaj mysza nad oknem konsoli")
    out("   - kliknij LPM 2 razy i PPM 1 raz")
    out("   - przewin kolkiem")
    out("   - zmien raz rozmiar okna (przeciągnij krawędź)")
    out("  Surowe sekwencje beda widoczne na ekranie - to normalne.")

    raw_chunks = []
    t_end = time.time() + 15
    while time.time() < t_end:
        if msvcrt.kbhit():
            try:
                n = msvcrt.getwch()
                chunk = n
                # doklej reszte sekwencji
                guard = 0
                while guard < 32:
                    guard += 1
                    if not msvcrt.kbhit():
                        # sekwencje moga dochodzic partiami - poczekaj chwile
                        if chunk.startswith("\x1b") and not re.search(r"[A-Za-z~Mm]$", chunk):
                            time.sleep(0.004)
                            if msvcrt.kbhit():
                                chunk += msvcrt.getwch()
                                continue
                        break
                    chunk += msvcrt.getwch()
                raw_chunks.append(chunk)
            except Exception as exc:
                R.setdefault("read_errors", []).append(repr(exc))
                break
        else:
            time.sleep(0.01)

    joined = "".join(raw_chunks)
    R["raw"] = {
        "chunks": len(raw_chunks),
        "bytes_total": len(joined),
        "head_repr": repr(joined[:600]),
    }

    sgr = SEQ_RE.findall(joined)
    x10 = X10_RE.findall(joined)
    resize = RESIZE_RE.findall(joined)
    keys = KEY_RE.findall(joined)

    buttons = {}
    for (b, x, y, term) in sgr:
        buttons[b] = buttons.get(b, 0) + 1

    R["parsed"] = {
        "sgr_events": len(sgr),
        "sgr_first_10": [list(map(str, e)) for e in sgr[:10]],
        "sgr_button_codes": buttons,
        "x10_markers": len(x10),
        "resize_events": [list(map(int, e)) for e in resize],
        "csi_key_sequences": len(keys),
        "csi_key_first_10": [list(map(str, e)) for e in keys[:10]],
        "contains_esc_byte": "\x1b" in joined,
    }

    # --- wylacz raportowanie ---
    write_fd0_flush("\x1b[?1015l\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l")
    write_fd0_flush("\x1b[0m")

    # --- przywroc tryb konsoli ---
    try:
        import ctypes
        k32 = ctypes.WinDLL("kernel32")
        hin = k32.GetStdHandle(-10)
        if R.get("input_mode_before"):
            k32.SetConsoleMode(hin, int(R["input_mode_before"], 16))
    except Exception as exc:
        R["restore_error"] = repr(exc)

    # --- interpretacja kodow przyciskow SGR ---
    # bit 0 = LPM, bit 1 = PPM, bit 2 = SPM, bit 3 = ruch, bity 6/7 = kolko
    interp = []
    for code in sorted(buttons, key=lambda c: int(c)):
        c = int(code)
        what = []
        if c & 64:
            what.append("kolo_w_gore" if c & 1 else "kolo_w_dol")
        else:
            if c & 1:
                what.append("PPM" if (c & 2) else "LPM")
            elif c & 2:
                what.append("SPM")
            else:
                what.append("brak_przycisku")
            if c & 32:
                what.append("ruch")
            if c & 4:
                what.append("SHIFT")
            if c & 8:
                what.append("META")
            if c & 16:
                what.append("CTRL")
            what.append("release" if (c & 3) == 3 else "press")
        interp.append({"code": c, "count": buttons[code], "meaning": "+".join(what)})
    R["parsed"]["button_interpretation"] = interp

    # --- werdykt ---
    v = {
        "host_is_conemu": is_conemu,
        "sgr_mouse_works": len(sgr) > 0,
        "any_mouse_reporting": len(sgr) > 0 or len(x10) > 0,
        "resize_reporting_works": len(resize) > 0,
        "buttons_seen": [i["meaning"] for i in interp],
        "tui_design": None,
    }
    if v["sgr_mouse_works"] and v["resize_reporting_works"]:
        v["tui_design"] = ("Textual z pelna obsluga myszy: klikalna lista gier, "
                           "przewijanie kolkiem, reakcja na resize. Najlepszy wariant.")
    elif v["sgr_mouse_works"]:
        v["tui_design"] = ("Textual z mysza, ale resize trzeba brac z ReadConsoleInputW "
                           "(zdarzenie typu 4) albo z GetConsoleScreenBufferInfo przy kazdej petli.")
    elif v["any_mouse_reporting"]:
        v["tui_design"] = ("Mysz w formacie X10 (nie SGR) - Textual to obsluguje, ale "
                           "bez coordinates release. Klikalna lista gier dziala.")
    else:
        v["tui_design"] = ("BRAK raportowania myszy przez ANSI. TUI musi byc nawigowane "
                           "klawiatura (strzalki/Enter/Esc/litery) albo czytac mysz bezposrednio "
                           "przez ReadConsoleInputW w wasnym backendzie. T3 potwierdzil, ze "
                           "ReadConsoleInputW daje 146 zdarzen myszy w 20 s - wiec klawiatura "
                           "to fallback, a mysz przez Win32 API to opcja.")
    R["verdict"] = v

    out("")
    out("=== WERDYKT T7 ===")
    out(json.dumps(v, indent=1, ensure_ascii=False))
    out("")
    out("  surowe dane (pierwsze 300): %r" % joined[:300])

    _save()
    return 0


def _save():
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(R, fh, indent=1, ensure_ascii=False)
    out("")
    out("Zapisano: %s" % OUT)
    out("Przeslij mi ten plik.")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        import traceback
        traceback.print_exc()
        R["fatal"] = traceback.format_exc()
        try:
            _save()
        except Exception:
            pass
        sys.exit(1)
