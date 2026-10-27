# -*- coding: utf-8 -*-
"""
T5 - Kto tak naprawde parsuje ANSI na tej maszynie? (ConEmu vs czysty conhost)

T3 wykazal, ze ANSI DZIALA (ESC znika, atrybut 0x4 sie pojawia), ale jednoczesnie
SetConsoleMode(ENABLE_VIRTUAL_TERMINAL_PROCESSING) zwrocilo ERROR_INVALID_PARAMETER
(87) - czyli samo API nie umie wlaczyc VT. Do tego w HKCU\\Console sa klucze ConEmu,
a GetNumberOfConsoleMouseButtons zwrocilo 16 (conhost zwraca 2). To silnie wskazuje,
 ze test lecial WEWNATRZ ConEmu, ktory przechwytuje API konsoli przez ConEmuHk64.dll.

To jest decydujace dla projektu:
  - jesli VT daje ConEmu  -> TUI musi startowac przez ConEmu (albo musimy miec
    fallback na wlasny backend Win32 dla czystego cmd.exe)
  - jesli VT daje conhost (np. modyfikacja JG 2021) -> TUI dziala wszedzie

Test nic nie instaluje i nie zmienia trwale. Uruchom go DWA RAZY:

  1) w tym terminalu, w ktorym lecial T3 (prawdopodobnie ConEmu)
  2) w czystym cmd.exe:  Start -> Uruchom -> cmd.exe
       cd /d "%~dp0"
       python T5_host_detect.py

Kazde uruchomienie zapisuje osobny plik (T5_wynik_1.json, T5_wynik_2.json, ...)
i wypisuje nazwe pliku na koncu. Przeslij mi OBYDWA.
"""

import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))

R = {"test": "T5_host_detect",
     "when_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
     "python": sys.version,
     "argv": sys.argv}


def out(m):
    try:
        sys.stdout.write(m + "\n")
    except UnicodeEncodeError:
        sys.stdout.write(m.encode("ascii", "replace").decode("ascii") + "\n")
    sys.stdout.flush()


def wmic_lines(args):
    import subprocess
    try:
        p = subprocess.Popen(["wmic"] + args, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, universal_newlines=True,
                             errors="replace")
        o, e = p.communicate()
        return [ln.strip() for ln in (o or "").splitlines() if ln.strip()]
    except Exception as exc:
        return ["wmic failed: %r" % (exc,)]


def main():
    # ---------- 1. zmienne srodowiskowe ----------
    interesting = ("ConEmu", "ANSICON", "TERM", "WT_", "Conhost", "PROMPT",
                   "ComSpec", "MSYSTEM", "MINGW", "SHTERM", "LC_", "VTE", "COLORTERM")
    env = {}
    for k, v in os.environ.items():
        if any(k.upper().startswith(p.rstrip("_").upper()) or p.rstrip("_").upper() in k.upper()
               for p in interesting):
            env[k] = v
    R["env_filtered"] = env
    R["env_conemu_keys"] = sorted(k for k in os.environ if k.lower().startswith("conemu"))
    out("=== Zmienne srodowiskowe (filtrowane) ===")
    for k in sorted(env):
        out("  %-28s = %s" % (k, env[k][:120]))
    out("  ConEmu* keys: %s" % (R["env_conemu_keys"] or "BRAK"))

    # ---------- 2. tytul konsoli ----------
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.GetConsoleTitleW.restype = wintypes.DWORD
    k32.GetConsoleTitleW.argtypes = [wintypes.LPWSTR, wintypes.DWORD]
    k32.GetConsoleWindow.restype = wintypes.HWND
    buf = ctypes.create_unicode_buffer(1024)
    n = k32.GetConsoleTitleW(buf, 1024)
    R["console_title"] = buf.value if n else None

    hwnd = k32.GetConsoleWindow()
    cls = ctypes.create_unicode_buffer(256)
    user32 = ctypes.windll.user32
    user32.GetClassNameW(hwnd, cls, 256)
    R["console_window_class"] = cls.value
    R["console_hwnd"] = str(hwnd)

    # nazwa procesu wlasciciela okna konsoli
    pid = wintypes.DWORD(0)
    try:
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        R["console_window_pid"] = pid.value
    except Exception as exc:
        R["console_window_pid_error"] = repr(exc)

    out("=== Okno konsoli ===")
    out("  tytul = %r" % R["console_title"])
    out("  klasa = %r   pid = %s" % (R["console_window_class"], R.get("console_window_pid")))

    # ---------- 3. drzewo procesow ----------
    R["process_tree"] = {}
    R["process_tree"]["tasklist_conemu"] = [l for l in wmic_lines(
        ["process", "where", "name like '%conemu%'", "get",
         "ProcessId,ParentProcessId,Name,ExecutablePath", "/format:list"])][:40]
    R["process_tree"]["tasklist_conhost"] = [l for l in wmic_lines(
        ["process", "where", "name='conhost.exe'", "get",
         "ProcessId,ParentProcessId,Name", "/format:list"])][:20]
    R["process_tree"]["self"] = [l for l in wmic_lines(
        ["process", "where", "ProcessId=%d" % os.getpid(), "get",
         "ProcessId,ParentProcessId,Name,ExecutablePath", "/format:list"])][:20]
    parent = None
    try:
        for l in R["process_tree"]["self"]:
            if l.startswith("ParentProcessId="):
                parent = int(l.split("=", 1)[1])
    except Exception:
        pass
    R["process_tree"]["parent_pid"] = parent
    if parent:
        R["process_tree"]["parent"] = [l for l in wmic_lines(
            ["process", "where", "ProcessId=%d" % parent, "get",
             "ProcessId,ParentProcessId,Name,ExecutablePath", "/format:list"])][:20]
        gp = None
        try:
            for l in R["process_tree"]["parent"]:
                if l.startswith("ParentProcessId="):
                    gp = int(l.split("=", 1)[1])
        except Exception:
            pass
        if gp:
            R["process_tree"]["grandparent"] = [l for l in wmic_lines(
                ["process", "where", "ProcessId=%d" % gp, "get",
                 "ProcessId,Name,ExecutablePath", "/format:list"])][:20]
    out("=== Drzewo procesow ===")
    for k in ("self", "parent", "grandparent"):
        out("  %s: %s" % (k, R["process_tree"].get(k)))
    out("  conemu w tasklist: %s" % (R["process_tree"]["tasklist_conemu"] or "BRAK"))

    # ---------- 4. moduly zaladowane w TYM procesie (hook ConEmu?) ----------
    mods = []
    try:
        psapi = ctypes.WinDLL("psapi")
        psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
        psapi.GetModuleFileNameExW.restype = wt.DWORD
        k32.GetCurrentProcess.restype = wt.HANDLE
        psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p),
                                               wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
        h = k32.GetCurrentProcess()
        arr = (ctypes.c_void_p * 1024)()
        need = wt.DWORD(0)
        LIST_MODULES_ALL = 0x03
        if psapi.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), LIST_MODULES_ALL):
            cnt = min(1024, need.value // ctypes.sizeof(ctypes.c_void_p))
            namebuf = ctypes.create_unicode_buffer(1024)
            for i in range(cnt):
                if psapi.GetModuleFileNameExW(h, arr[i], namebuf, 1024):
                    mods.append(namebuf.value)
    except Exception as exc:
        R["modules_error"] = repr(exc)
    R["loaded_modules_count"] = len(mods)
    R["loaded_modules_suspicious"] = [m for m in mods if any(
        t in m.lower() for t in ("conemu", "hk64", "hk32", "ansicon", "cmdex", "vds", "clink"))]
    R["loaded_modules_all"] = mods
    out("=== Moduly w procesie ===")
    out("  lacznie=%d  podejrzane(hook)=%s" % (len(mods), R["loaded_modules_suspicious"] or "BRAK"))

    # ---------- 5. empiryczny test ANSI + truecolor + SGR mouse ----------
    hout = k32.GetStdHandle(0xFFFFFFF5)
    COORD = wintypes.SHORT * 2

    class COORD_S(ctypes.Structure):
        _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]

    k32.GetConsoleMode.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
    k32.SetConsoleMode.argtypes = [wt.HANDLE, wt.DWORD]
    k32.SetConsoleCursorPosition.argtypes = [wt.HANDLE, COORD_S]
    k32.ReadConsoleOutputAttribute.argtypes = [wt.HANDLE, ctypes.POINTER(wt.WORD),
                                               wt.DWORD, COORD_S, ctypes.POINTER(wt.DWORD)]
    k32.ReadConsoleOutputCharacterW.argtypes = [wt.HANDLE, wt.LPWSTR, wt.DWORD, COORD_S,
                                                ctypes.POINTER(wt.DWORD)]
    k32.WriteConsoleOutputCharacterW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.DWORD, COORD_S,
                                                 ctypes.POINTER(wt.DWORD)]

    m = wt.DWORD(0)
    k32.GetConsoleMode(hout, ctypes.byref(m))
    R["output_mode_before"] = hex(m.value)

    # ustaw kursor na linii 3 i wyczysc ja
    row = 3
    w = wt.DWORD(0)
    k32.WriteConsoleOutputCharacterW(hout, " " * 70, 70, COORD_S(0, row), ctypes.byref(w))
    k32.SetConsoleCursorPosition(hout, COORD_S(0, row))

    # (a) 16 kolorow
    sys.stdout.write("\x1b[31mRED16\x1b[0m")
    # (b) 256 kolorow
    sys.stdout.write("\x1b[38;5;208mORANGE256\x1b[0m")
    # (c) truecolor
    sys.stdout.write("\x1b[38;2;0;200;255mTRUECOLOR\x1b[0m")
    # (d) bold
    sys.stdout.write("\x1b[1mBOLD\x1b[0m")
    sys.stdout.flush()
    time.sleep(0.4)

    cb = ctypes.create_unicode_buffer(71)
    ab = (wt.WORD * 70)()
    nr = wt.DWORD(0)
    k32.ReadConsoleOutputCharacterW(hout, cb, 70, COORD_S(0, row), ctypes.byref(nr))
    k32.ReadConsoleOutputAttribute(hout, ab, 70, COORD_S(0, row), ctypes.byref(nr))
    chars = cb.value
    attrs = list(ab[:70])
    R["ansi16_truecolor"] = {
        "chars": chars,
        "attrs_hex": [hex(a) for a in attrs[:40]],
        "escape_leaked_as_text": ("\x1b" in chars) or ("[31m" in chars) or ("[38;" in chars),
    }

    def fg_at(idx):
        return attrs[idx] & 0x0F if idx < len(attrs) else None

    R["ansi16_truecolor"]["fg_at_0"] = hex(fg_at(0)) if fg_at(0) is not None else None
    R["ansi16_truecolor"]["fg_at_5"] = hex(fg_at(5)) if fg_at(5) is not None else None
    R["ansi16_truecolor"]["fg_at_16"] = hex(fg_at(16)) if fg_at(16) is not None else None
    # RED16=5 znakow (0-4), ORANGE256=9 (5-13), TRUECOLOR=9 (14-22), BOLD=4 (23-26)
    R["ansi16_truecolor"]["red16_ok"] = fg_at(0) == 0x04
    R["ansi16_truecolor"]["color256_distinct"] = (fg_at(5) is not None and fg_at(5) != fg_at(0))
    R["ansi16_truecolor"]["truecolor_distinct"] = (fg_at(14) is not None and fg_at(14) != fg_at(5))
    R["ansi16_truecolor"]["bold_flag"] = bool(attrs[23] & 0x08) if len(attrs) > 23 else None

    out("=== ANSI: 16 / 256 / truecolor ===")
    out("  odczyt: %r" % chars[:60])
    out("  ESC wyciekl jako tekst : %s" % R["ansi16_truecolor"]["escape_leaked_as_text"])
    out("  RED16 (fg=0x4)         : %s" % R["ansi16_truecolor"]["red16_ok"])
    out("  256-kolor rozny od 16  : %s" % R["ansi16_truecolor"]["color256_distinct"])
    out("  truecolor rozny od 256 : %s" % R["ansi16_truecolor"]["truecolor_distinct"])
    out("  atrybuty[0:30]         : %s" % [hex(a) for a in attrs[:30]])

    # (e) alt screen + cursor addressing + erase
    k32.WriteConsoleOutputCharacterW(hout, " " * 70, 70, COORD_S(0, row), ctypes.byref(w))
    sys.stdout.write("\x1b[?1049h")            # alt screen
    sys.stdout.write("\x1b[2J\x1b[H")           # clear + home
    sys.stdout.write("\x1b[10;20HALTSCREEN_POS_OK")
    sys.stdout.flush()
    time.sleep(0.35)
    cb2 = ctypes.create_unicode_buffer(40)
    ok_read = bool(k32.ReadConsoleOutputCharacterW(hout, cb2, 40, COORD_S(19, 9), ctypes.byref(nr)))
    R["alt_screen"] = {"read_ok": ok_read, "cell_at_10_20": cb2.value}
    R["alt_screen"]["cursor_addressing_ok"] = cb2.value.startswith("ALTSCREEN_POS_OK")
    sys.stdout.write("\x1b[?1049l")
    sys.stdout.flush()
    out("=== Alt-screen + adresowanie kursora ===")
    out("  komorka (10,20) = %r  -> adresowanie dziala = %s"
        % (cb2.value[:30], R["alt_screen"]["cursor_addressing_ok"]))

    # (f) SGR mouse (1006) - tylko sprawdzamy, czy host przyjmuje sekwencje
    sys.stdout.write("\x1b[?1000h\x1b[?1006h")
    sys.stdout.flush()
    R["sgr_mouse_enabled_emitted"] = True
    out("=== Mysz SGR ===")
    out("  Wlaczylem raportowanie myszy (1000+1006).")
    out("  Przez 6 sekund PORUSZAJ mysza nad oknem konsoli i KLIKNIJ 2 razy.")
    out("  Jesli host wspiera SGR, zobaczysz surowe sekwencje typu '<0;25;12M'.")
    sgr = []
    try:
        t_end = time.time() + 6
        while time.time() < t_end:
            if msvcrt_kbhit():
                ch = read_key_chunk()
                if ch:
                    sgr.append(ch)
            else:
                time.sleep(0.02)
    except Exception as exc:
        R["sgr_error"] = repr(exc)
    sys.stdout.write("\x1b[?1006l\x1b[?1000l")
    sys.stdout.flush()
    joined = "".join(sgr)
    R["sgr_mouse"] = {"raw_chunks": len(sgr), "joined_head": joined[:300],
                      "looks_like_sgr": ("\x1b[<" in joined)}
    out("  surowe dane: %r" % joined[:200])
    out("  wyglada jak SGR (ESC[<) : %s" % R["sgr_mouse"]["looks_like_sgr"])

    # ---------- 6. werdykt ----------
    a = R["ansi16_truecolor"]
    v = {
        "host_conemu_present_in_env": bool(R["env_conemu_keys"]),
        "hook_dll_loaded": bool(R["loaded_modules_suspicious"]),
        "console_window_class": R.get("console_window_class"),
        "vt_16color_works": bool(a["red16_ok"] and not a["escape_leaked_as_text"]),
        "vt_256_works": bool(a["color256_distinct"]),
        "vt_truecolor_works": bool(a["truecolor_distinct"]),
        "alt_screen_and_cursor_addressing": bool(R["alt_screen"]["cursor_addressing_ok"]),
        "sgr_mouse": bool(R["sgr_mouse"]["looks_like_sgr"]),
    }
    if v["vt_16color_works"] and v["alt_screen_and_cursor_addressing"]:
        if v["host_conemu_present_in_env"] or v["hook_dll_loaded"]:
            v["conclusion"] = ("VT zapewnia ConEmu (hook DLL / env). Textual zadziala, ale TYLKO "
                               "gdy program startuje przez ConEmu. Czysty cmd.exe trzeba zmierzyc "
                               "osobno (drugie uruchomienie tego testu).")
        else:
            v["conclusion"] = ("VT zapewnia sam conhost/Windows (bez ConEmu w env i bez hook DLL). "
                               "Textual zadziala rowniez w czystym cmd.exe - to najlepszy mozliwy wynik.")
    else:
        v["conclusion"] = "VT NIE dziala w tym hostie - potrzebny wlasny backend Win32 albo ConEmu."
    R["verdict"] = v
    out("")
    out("=== WERDYKT T5 ===")
    out(json.dumps(v, indent=1, ensure_ascii=False))

    # ---------- zapis ----------
    idx = 1
    while os.path.exists(os.path.join(HERE, "T5_wynik_%d.json" % idx)):
        idx += 1
    path = os.path.join(HERE, "T5_wynik_%d.json" % idx)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(R, fh, indent=1, ensure_ascii=False)
    out("")
    out("Zapisano: %s" % path)
    out("Przeslij mi ten plik. Potem uruchom ten sam test w CZYSTYM cmd.exe i przeslij drugi.")
    return 0


def msvcrt_kbhit():
    import msvcrt
    return msvcrt.kbhit()


def read_key_chunk():
    """Czyta zalegle znaki z konsoli (sekwencje SGR sa wieloznakowe: ESC [ < 0 ; 2 5 ; 1 2 M)."""
    import msvcrt
    chunk = []
    guard = 0
    while msvcrt.kbhit() and guard < 64:
        guard += 1
        c = msvcrt.getwch()
        chunk.append(c)
        if c in ("\x00", "\xe0") and msvcrt.kbhit():
            chunk.append(msvcrt.getwch())
        # po ESC probuj dobrac reszte sekwencji, nawet jesli jeszcze nie doszla
        if c == "\x1b":
            t0 = time.time()
            while time.time() - t0 < 0.06 and guard < 64:
                if not msvcrt.kbhit():
                    time.sleep(0.005)
                    continue
                c2 = msvcrt.getwch()
                chunk.append(c2)
                guard += 1
                if c2 in ("M", "m") or (c2.isalpha() and c2 not in ("[", "<")):
                    break
    return "".join(chunk)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        import traceback
        traceback.print_exc()
        R["fatal"] = traceback.format_exc()
        try:
            with open(os.path.join(HERE, "T5_wynik_blad.json"), "w", encoding="utf-8") as fh:
                json.dump(R, fh, indent=1, ensure_ascii=False)
        except Exception:
            pass
        sys.exit(1)
