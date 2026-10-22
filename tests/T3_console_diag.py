# -*- coding: utf-8 -*-
"""
T3 - Diagnostyka konsoli Windows: czy na tej maszynie da sie zrobic TUI w Pythonie?

Kontekst: conhost Windows 7 nie parsuje sekwencji ANSI/VT (potwierdzone m.in.
w Textualize/textual#3378 na dokladnie tej samej konfiguracji: Win7 x64 + cmd +
Python 3.8.10). Textual i rich.Live wymagaja VT. Ten test mierzy faktyczny stan
na TWOJEJ maszynie - modyfikacja JG 2021 moze cos zmieniac, wiec nic nie zakladamy.

Testy:
  A  python / ssl / kodowania
  B  tryby uchwytu konsoli + ENABLE_VIRTUAL_TERMINAL_PROCESSING (0x0004)
  C  test EMPIRYCZNY ANSI: piszemy sekwencje, potem czytamy komorki przez
     ReadConsoleOutputCharacterW/Attribute i sprawdzamy, czy kolor zostal
     zastosowany, czy ESC wyciekl jako tekst
  D  alternatywny bufor ekranu (CreateConsoleScreenBuffer vs ANSI 1049)
  E  zapis/odczyt znakow Unicode (ramki, bloki) przez WriteConsoleOutputCharacterW
  F  wejscie: WaitForSingleObject + ReadConsoleInputW (klawiatura I MYSZ)
  G  geometria, codepages, fonty konsoli z rejestru
  H  wydajnosc WriteConsoleOutputW (ile kosztuje przepelnienie calych 80x25)
  I  co widzi rich (legacy_windows / vt / truecolor)

Wymagania: Windows, python 3.6+. Czas: ok. 35 sekund (w tym 20 s interakcji).

Uruchomienie:   python T3_console_diag.py
Wynik:          T3_wynik.json   <- przeslij mi ten plik
"""

import json
import os
import platform
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_JSON = os.path.join(HERE, "T3_wynik.json")

RESULT = {
    "test": "T3_console_diag",
    "when_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
}


def out(msg):
    try:
        sys.stdout.write(msg + "\n")
    except UnicodeEncodeError:
        sys.stdout.write((msg + "\n").encode("ascii", "replace").decode("ascii"))
    sys.stdout.flush()


def dump():
    with open(OUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(RESULT, fh, indent=1, ensure_ascii=False)
    out("")
    out("Wynik zapisany: %s" % OUT_JSON)
    out("Przeslij mi ten plik (albo jego cala zawartosc).")


def main():
    # ---------------- A. python ----------------
    RESULT["python"] = {
        "version": sys.version,
        "version_info": list(sys.version_info),
        "executable": sys.executable,
        "is_64bit": sys.maxsize > 2 ** 32,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "stdout_encoding": getattr(sys.stdout, "encoding", None),
        "filesystem_encoding": sys.getfilesystemencoding(),
        "stdin_isatty": bool(sys.stdin.isatty()),
        "stdout_isatty": bool(sys.stdout.isatty()),
        "argv": sys.argv,
    }
    try:
        import ssl
        RESULT["python"]["ssl_openssl"] = ssl.OPENSSL_VERSION
    except Exception as exc:
        RESULT["python"]["ssl_error"] = repr(exc)

    if os.name != "nt":
        RESULT["fatal"] = "not windows"
        dump()
        return 2

    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    # ---- struktury ----
    class COORD(ctypes.Structure):
        _fields_ = [("X", wintypes.SHORT), ("Y", wintypes.SHORT)]

    class SMALL_RECT(ctypes.Structure):
        _fields_ = [("Left", wintypes.SHORT), ("Top", wintypes.SHORT),
                    ("Right", wintypes.SHORT), ("Bottom", wintypes.SHORT)]

    class CONSOLE_SCREEN_BUFFER_INFO(ctypes.Structure):
        _fields_ = [("dwSize", COORD), ("dwCursorPosition", COORD),
                    ("wAttributes", wintypes.WORD), ("srWindow", SMALL_RECT),
                    ("dwMaximumWindowSize", COORD)]

    class CHAR_INFO(ctypes.Structure):
        _fields_ = [("UnicodeChar", wintypes.WCHAR), ("Attributes", wintypes.WORD)]

    class KEY_EVENT_RECORD(ctypes.Structure):
        _fields_ = [("bKeyDown", wintypes.BOOL), ("wRepeatCount", wintypes.WORD),
                    ("wVirtualKeyCode", wintypes.WORD), ("wVirtualScanCode", wintypes.WORD),
                    ("uChar", ctypes.c_ushort), ("dwControlKeyState", wintypes.DWORD)]

    class MOUSE_EVENT_RECORD(ctypes.Structure):
        _fields_ = [("dwMousePosition", COORD), ("dwButtonState", wintypes.DWORD),
                    ("dwControlKeyState", wintypes.DWORD), ("dwEventFlags", wintypes.DWORD)]

    class WINDOW_BUFFER_SIZE_RECORD(ctypes.Structure):
        _fields_ = [("dwSize", COORD)]

    class FOCUS_RECORD(ctypes.Structure):
        _fields_ = [("bSetFocus", wintypes.BOOL)]

    class EVENT_UNION(ctypes.Union):
        # UWAGA: nazwy pol nie moga sie pokrywac prefiksowo (ctypes to odrzuca)
        _fields_ = [("KeyEvent", KEY_EVENT_RECORD),
                    ("MouseEvent", MOUSE_EVENT_RECORD),
                    ("WindowBufferSizeEvent", WINDOW_BUFFER_SIZE_RECORD),
                    ("FocusEvent", FOCUS_RECORD),
                    ("raw", ctypes.c_byte * 24)]

    class INPUT_RECORD(ctypes.Structure):
        _fields_ = [("EventType", wintypes.WORD), ("Event", EVENT_UNION)]

    # ---- prototypy ----
    k32.GetStdHandle.restype = wintypes.HANDLE
    k32.GetStdHandle.argtypes = [wintypes.DWORD]
    k32.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    k32.GetConsoleMode.restype = wintypes.BOOL
    k32.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.SetConsoleMode.restype = wintypes.BOOL
    k32.GetConsoleScreenBufferInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(CONSOLE_SCREEN_BUFFER_INFO)]
    k32.GetConsoleScreenBufferInfo.restype = wintypes.BOOL
    k32.SetConsoleCursorPosition.argtypes = [wintypes.HANDLE, COORD]
    k32.WriteConsoleOutputCharacterW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR,
                                                 wintypes.DWORD, COORD, ctypes.POINTER(wintypes.DWORD)]
    k32.WriteConsoleOutputCharacterW.restype = wintypes.BOOL
    k32.ReadConsoleOutputCharacterW.argtypes = [wintypes.HANDLE, wintypes.LPWSTR,
                                                wintypes.DWORD, COORD, ctypes.POINTER(wintypes.DWORD)]
    k32.ReadConsoleOutputCharacterW.restype = wintypes.BOOL
    k32.ReadConsoleOutputAttribute.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.WORD),
                                               wintypes.DWORD, COORD, ctypes.POINTER(wintypes.DWORD)]
    k32.ReadConsoleOutputAttribute.restype = wintypes.BOOL
    k32.WriteConsoleOutputW.argtypes = [wintypes.HANDLE, ctypes.POINTER(CHAR_INFO), COORD,
                                        COORD, ctypes.POINTER(SMALL_RECT)]
    k32.WriteConsoleOutputW.restype = wintypes.BOOL
    k32.CreateConsoleScreenBuffer.restype = wintypes.HANDLE
    k32.CreateConsoleScreenBuffer.argtypes = [wintypes.DWORD, wintypes.DWORD,
                                              wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID]
    k32.SetConsoleActiveScreenBuffer.argtypes = [wintypes.HANDLE]
    k32.SetConsoleActiveScreenBuffer.restype = wintypes.BOOL
    k32.ReadConsoleInputW.argtypes = [wintypes.HANDLE, ctypes.POINTER(INPUT_RECORD),
                                      wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    k32.ReadConsoleInputW.restype = wintypes.BOOL
    k32.GetNumberOfConsoleMouseButtons.argtypes = [ctypes.POINTER(wintypes.DWORD)]
    k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k32.WaitForSingleObject.restype = wintypes.DWORD
    k32.GetConsoleCP.restype = wintypes.UINT
    k32.GetConsoleOutputCP.restype = wintypes.UINT

    STD_INPUT_HANDLE = 0xFFFFFFF6   # -10
    STD_OUTPUT_HANDLE = 0xFFFFFFF5  # -11
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 1
    FILE_SHARE_WRITE = 2
    CONSOLE_TEXTMODE_BUFFER = 1
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    ENABLE_PROCESSED_INPUT = 0x0001
    ENABLE_LINE_INPUT = 0x0002
    ENABLE_ECHO_INPUT = 0x0004
    ENABLE_WINDOW_INPUT = 0x0008
    ENABLE_MOUSE_INPUT = 0x0010
    ENABLE_INSERT_MODE = 0x0020
    ENABLE_QUICK_EDIT_MODE = 0x0040
    ENABLE_EXTENDED_FLAGS = 0x0080
    ENABLE_PROCESSED_OUTPUT = 0x0001
    ENABLE_WRAP_AT_EOL_OUTPUT = 0x0002
    ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
    DISABLE_NEWLINE_AUTO_RETURN = 0x0008
    ENABLE_LVB_GRID_WORLDWIDE = 0x0010

    hout = k32.GetStdHandle(STD_OUTPUT_HANDLE)
    hin = k32.GetStdHandle(STD_INPUT_HANDLE)
    RESULT["handles"] = {"stdout": str(hout), "stdin": str(hin),
                         "stdout_valid": bool(hout) and hout != INVALID_HANDLE_VALUE,
                         "stdin_valid": bool(hin) and hin != INVALID_HANDLE_VALUE}
    if not RESULT["handles"]["stdout_valid"]:
        RESULT["fatal"] = "stdout is not a console handle (przekierowanie? uruchom bez > plik)"
        dump()
        return 2

    def mode_of(h):
        m = wintypes.DWORD(0)
        ok = bool(k32.GetConsoleMode(h, ctypes.byref(m)))
        return (ok, m.value)

    # ---------------- B. tryby ----------------
    ok_o, mo = mode_of(hout)
    ok_i, mi = mode_of(hin)
    vt = {"output_mode_ok": ok_o, "output_mode": hex(mo) if ok_o else None,
          "input_mode_ok": ok_i, "input_mode": hex(mi) if ok_i else None,
          "flag_values": {
              "ENABLE_VIRTUAL_TERMINAL_PROCESSING": ENABLE_VIRTUAL_TERMINAL_PROCESSING,
              "ENABLE_PROCESSED_OUTPUT": ENABLE_PROCESSED_OUTPUT,
              "ENABLE_WRAP_AT_EOL_OUTPUT": ENABLE_WRAP_AT_EOL_OUTPUT}}
    if ok_o:
        want = mo | ENABLE_VIRTUAL_TERMINAL_PROCESSING
        set_ok = bool(k32.SetConsoleMode(hout, want))
        ok_a, ma = mode_of(hout)
        vt["set_vt_call_ok"] = set_ok
        vt["mode_after_set"] = hex(ma) if ok_a else None
        vt["vt_flag_persisted"] = bool(ok_a and (ma & ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        vt["last_error_if_failed"] = ctypes.get_last_error() if not vt["vt_flag_persisted"] else None
        k32.SetConsoleMode(hout, mo)  # przywroc
    RESULT["vt_capability"] = vt
    out("=== B. Tryby konsoli ===")
    out("  output mode = %s   input mode = %s" % (vt.get("output_mode"), vt.get("input_mode")))
    out("  SetConsoleMode(VT_PROCESSING): call_ok=%s  persisted=%s  (err=%s)"
        % (vt.get("set_vt_call_ok"), vt.get("vt_flag_persisted"), vt.get("last_error_if_failed")))

    # ---------------- G. geometria ----------------
    csbi = CONSOLE_SCREEN_BUFFER_INFO()
    ok = bool(k32.GetConsoleScreenBufferInfo(hout, ctypes.byref(csbi)))
    geom = {"ok": ok}
    row = 6
    if ok:
        geom["buffer_size"] = [csbi.dwSize.X, csbi.dwSize.Y]
        geom["cursor_pos"] = [csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y]
        geom["attributes"] = hex(csbi.wAttributes)
        geom["window_rect"] = [csbi.srWindow.Left, csbi.srWindow.Top, csbi.srWindow.Right, csbi.srWindow.Bottom]
        geom["window_cells"] = [csbi.srWindow.Right - csbi.srWindow.Left + 1,
                                csbi.srWindow.Bottom - csbi.srWindow.Top + 1]
        geom["max_window_cells"] = [csbi.dwMaximumWindowSize.X, csbi.dwMaximumWindowSize.Y]
        row = max(2, min(csbi.dwSize.Y - 3, csbi.dwCursorPosition.Y))
    nbtn = wintypes.DWORD(0)
    geom["mouse_buttons_ok"] = bool(k32.GetNumberOfConsoleMouseButtons(ctypes.byref(nbtn)))
    geom["mouse_buttons"] = nbtn.value
    RESULT["geometry"] = geom
    RESULT["codepages"] = {"input_cp": k32.GetConsoleCP(), "output_cp": k32.GetConsoleOutputCP()}
    out("=== G. Geometria / codepages ===")
    out("  bufor=%s  okno=%s komorek  atrybuty=%s  CP in/out=%s/%s"
        % (geom.get("buffer_size"), geom.get("window_cells"), geom.get("attributes"),
           RESULT["codepages"]["input_cp"], RESULT["codepages"]["output_cp"]))

    def write_cells(text, r, c=0):
        w = wintypes.DWORD(0)
        return bool(k32.WriteConsoleOutputCharacterW(hout, text, len(text), COORD(c, r), ctypes.byref(w))), w.value

    def read_cells(n, r, c=0):
        cb = ctypes.create_unicode_buffer(n + 1)
        ab = (wintypes.WORD * n)()
        nr = wintypes.DWORD(0)
        okc = bool(k32.ReadConsoleOutputCharacterW(hout, cb, n, COORD(c, r), ctypes.byref(nr)))
        oka = bool(k32.ReadConsoleOutputAttribute(hout, ab, n, COORD(c, r), ctypes.byref(nr)))
        return (okc, cb.value, oka, [a for a in ab[:n]])

    # ---------------- C. ANSI empirycznie ----------------
    out("=== C. Test ANSI (patrz na ekran konsoli) ===")
    # baseline: atrybuty komorek zanim cokolwiek nadpiszemy
    _okb, _cb, _okab, attrs_before = read_cells(78, row)
    baseline_attr = attrs_before[0] if (_okab and attrs_before) else None

    # KONTROLA POZYTYWNA: czy sciezka Win32 (atrybuty bez ANSI) w ogole dziala?
    ctrl = (CHAR_INFO * 6)()
    for i, ch in enumerate("KONTRO"):
        ctrl[i].UnicodeChar = ch
        ctrl[i].Attributes = 0x0C  # czerwony na czarnym, bez ANSI
    rect_c = SMALL_RECT(60, row, 65, row)
    ctrl_ok = bool(k32.WriteConsoleOutputW(hout, ctrl, COORD(6, 1), COORD(60, row), ctypes.byref(rect_c)))
    _okc3, _cb3, _oka3, attrs_ctrl = read_cells(6, row, 60)
    ctrl_applied = bool(_oka3 and attrs_ctrl and (attrs_ctrl[0] & 0x0F) == 0x04)
    RESULT["win32_attribute_control"] = {
        "write_ok": ctrl_ok,
        "attr_readback": [hex(a) for a in attrs_ctrl[:6]] if _oka3 else None,
        "color_applied_without_ansi": ctrl_applied,
    }
    out("  kontrola: WriteConsoleOutputW z atrybutem 0x0C -> kolor zastosowany = %s" % ctrl_applied)

    write_cells(" " * 78, row)
    k32.SetConsoleCursorPosition(hout, COORD(0, row))
    payload = "\x1b[31mCZERWONY\x1b[0m \x1b[42mZIELONE_TLO\x1b[0m \x1b[1mBOLD\x1b[0m"
    try:
        sys.stdout.write(payload)
        sys.stdout.flush()
    except Exception as exc:
        RESULT.setdefault("errors", {})["ansi_write"] = repr(exc)
    time.sleep(0.35)
    okc, chars, oka, attrs = read_cells(78, row)
    escaped = ("\x1b" in chars) or ("[31m" in chars) or ("[42m" in chars) or ("[0m" in chars)
    fg0 = (attrs[0] & 0x0F) if (oka and attrs) else None
    bg0 = ((attrs[8] >> 4) & 0x0F) if (oka and len(attrs) > 8) else None
    color_applied = (fg0 == 0x04)
    RESULT["ansi_probe"] = {
        "read_chars_ok": okc, "read_attrs_ok": oka,
        "baseline_attr_before_write": hex(baseline_attr) if baseline_attr is not None else None,
        "chars_first_60": chars[:60],
        "attrs_first_16": [hex(a) for a in attrs[:16]],
        "escape_sequences_visible_as_text": escaped,
        "first_cell_fg": hex(fg0) if fg0 is not None else None,
        "cell8_bg": hex(bg0) if bg0 is not None else None,
        "color_applied_via_ansi": color_applied,
        "attrs_changed_vs_baseline": (baseline_attr is not None and oka and attrs and attrs[0] != baseline_attr),
        "vt_supported_empirical": bool(color_applied and not escaped),
        "note": "fg 0x4 = FOREGROUND_RED; ESC/[31m widoczne jako tekst => conhost nie ma parsera VT",
    }
    out("  odczytane znaki: %r" % chars[:60])
    out("  ANSI widoczne jako tekst = %s   kolor zastosowany = %s (fg=%s, oczekiwano 0x4)"
        % (escaped, color_applied, hex(fg0) if fg0 is not None else None))
    out("  >> WERDYKT: VT dziala = %s" % RESULT["ansi_probe"]["vt_supported_empirical"])
    write_cells(" " * 78, row)

    # ---------------- E. Unicode ----------------
    sample = "+--+ | \u2500\u2502\u250c\u2510\u2514\u2518 \u2588\u2591\u2592\u2593 \u25b6 \u2713 \u25ae\u25af"
    wok, wcount = write_cells(sample, row)
    time.sleep(0.15)
    okc2, back, _oka2, _a2 = read_cells(80, row)
    RESULT["unicode_probe"] = {
        "write_ok": wok, "write_count": wcount, "read_ok": okc2,
        "wrote": sample, "readback": back.rstrip(),
        "roundtrip_identical": (back.rstrip() == sample.rstrip()),
        "question_marks": back.count("?"),
        "replacement_or_nul": back.count("\x00"),
        "ascii_only_fallback": "+--+ | ----------------------------------- ########### > v []",
    }
    out("=== E. Unicode (ramki/bloki) ===")
    out("  roundtrip identyczny = %s   znaki '?' = %s"
        % (RESULT["unicode_probe"]["roundtrip_identical"], RESULT["unicode_probe"]["question_marks"]))
    out("  jesli False/duzo '?' -> TUI musi uzywac ramek ASCII")
    write_cells(" " * 78, row)

    # ---------------- D. alternatywny bufor ----------------
    d = {}
    newbuf = k32.CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           None, CONSOLE_TEXTMODE_BUFFER, None)
    d["create_ok"] = bool(newbuf) and newbuf != INVALID_HANDLE_VALUE
    d["create_last_error"] = None if d["create_ok"] else ctypes.get_last_error()
    if d["create_ok"]:
        d["set_active_ok"] = bool(k32.SetConsoleActiveScreenBuffer(newbuf))
        if d["set_active_ok"]:
            w2 = wintypes.DWORD(0)
            k32.WriteConsoleOutputCharacterW(newbuf, "ALTERNATYWNY BUFOR DZIALA", 25, COORD(0, 0), ctypes.byref(w2))
            time.sleep(0.7)
            d["restore_ok"] = bool(k32.SetConsoleActiveScreenBuffer(hout))
        k32.CloseHandle(newbuf)
    d["ansi_alt_screen_emitted_1049"] = None
    try:
        sys.stdout.write("\x1b[?1049h")
        sys.stdout.flush()
        time.sleep(0.35)
        # jesli alt-screen ANSI zadzialal, odczyt z oryginalnego bufora sie nie zmieni;
        # przywroc bezwarunkowo
        sys.stdout.write("\x1b[?1049l")
        sys.stdout.flush()
        d["ansi_alt_screen_emitted_1049"] = True
    except Exception as exc:
        d["ansi_alt_screen_error"] = repr(exc)
    RESULT["alt_screen"] = d
    out("=== D. Alternatywny bufor ===")
    out("  CreateConsoleScreenBuffer=%s  SetActive=%s  restore=%s  (err=%s)"
        % (d.get("create_ok"), d.get("set_active_ok"), d.get("restore_ok"), d.get("create_last_error")))

    # ---------------- H. wydajnosc WriteConsoleOutputW ----------------
    perf = {}
    try:
        W, H = 80, 25
        cells = (CHAR_INFO * (W * H))()
        palette = " .:-=+*#%@"
        for i in range(W * H):
            cells[i].UnicodeChar = palette[i % len(palette)]
            cells[i].Attributes = 0x07 | ((i % 8) << 0)
        sz = COORD(W, H)
        origin = COORD(0, 0)
        rect = SMALL_RECT(0, 0, W - 1, H - 1)
        t0 = time.time()
        iters = 100
        for _ in range(iters):
            k32.WriteConsoleOutputW(hout, cells, sz, origin, ctypes.byref(rect))
        dt = time.time() - t0
        perf["full_80x25_writes"] = iters
        perf["total_seconds"] = round(dt, 4)
        perf["ms_per_full_screen_write"] = round(dt * 1000.0 / iters, 3)
        perf["fps_equivalent"] = round(iters / dt, 1) if dt else None
        # teraz tylko jedna linia (typowy przypadek: roznice)
        rect1 = SMALL_RECT(0, 0, W - 1, 0)
        sz1 = COORD(W, 1)
        row_cells = (CHAR_INFO * W)()
        for i in range(W):
            row_cells[i].UnicodeChar = "#"
            row_cells[i].Attributes = 0x07
        t0 = time.time()
        for _ in range(iters):
            k32.WriteConsoleOutputW(hout, row_cells, sz1, COORD(0, 0), ctypes.byref(rect1))
        dt1 = time.time() - t0
        perf["ms_per_single_row_write"] = round(dt1 * 1000.0 / iters, 4)
        # i WriteConsoleOutputCharacterW dla porownania
        t0 = time.time()
        w3 = wintypes.DWORD(0)
        for _ in range(iters):
            k32.WriteConsoleOutputCharacterW(hout, "#" * W, W, COORD(0, 0), ctypes.byref(w3))
        dt2 = time.time() - t0
        perf["ms_per_row_via_character_api"] = round(dt2 * 1000.0 / iters, 4)
    except Exception as exc:
        perf["error"] = repr(exc)
    RESULT["write_perf"] = perf
    out("=== H. Wydajnosc zapisu do konsoli ===")
    out("  %s" % json.dumps(perf))
    write_cells(" " * 78, 0)

    # ---------------- I. rich/textual ----------------
    libs = {}
    try:
        from rich._windows import get_windows_console_features
        f = get_windows_console_features()
        libs["rich_available"] = True
        libs["rich_vt"] = bool(getattr(f, "vt", None))
        libs["rich_truecolor"] = bool(getattr(f, "truecolor", None))
        try:
            import rich
            libs["rich_version"] = rich.__version__
        except Exception:
            pass
        try:
            from rich._windows import _get_windows_console_features as _g  # noqa
        except Exception:
            pass
        try:
            from rich.console import Console
            c = Console()
            libs["rich_console_legacy_windows"] = bool(getattr(c, "legacy_windows", None))
            libs["rich_console_color_system"] = str(getattr(c, "color_system", None))
            libs["rich_console_size"] = list(c.size)
        except Exception as exc:
            libs["rich_console_error"] = repr(exc)
    except Exception as exc:
        libs["rich_available"] = False
        libs["rich_error"] = repr(exc)
    try:
        import textual
        libs["textual_version"] = textual.__version__
    except Exception:
        libs["textual_available"] = False
    try:
        libs["pip_list_snapshot"] = json.loads(
            __import__("subprocess").check_output([sys.executable, "-m", "pip", "list", "--format=json"],
                                                   stderr=__import__("subprocess").STDOUT).decode("utf-8", "replace"))
    except Exception as exc:
        libs["pip_list_error"] = repr(exc)
    RESULT["python_tui_libs"] = libs
    out("=== I. Biblioteki TUI ===")
    out("  rich=%s vt=%s legacy=%s color=%s textual=%s"
        % (libs.get("rich_version"), libs.get("rich_vt"), libs.get("rich_console_legacy_windows"),
           libs.get("rich_console_color_system"), libs.get("textual_version", libs.get("textual_available"))))

    # ---------------- F. wejscie (interaktywne) ----------------
    out("=== F. Test wejscia (20 s, interaktywny) ===")
    out("  Kliknij kilka razy MYSZA w oknie konsoli, wcisnij strzalki, Enter, F1, A, Z.")
    out("  Nie zamykaj okna. Test sam sie zakonczy.")
    f = {"events": [], "counts_by_type": {}, "deadline_seconds": 20}
    try:
        ok_i2, mi2 = mode_of(hin)
        want = (mi2 | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE
        f["input_mode_before"] = hex(mi2) if ok_i2 else None
        f["set_input_mode_ok"] = bool(k32.SetConsoleMode(hin, want)) if ok_i2 else False
        f["input_mode_during"] = hex(want)
        recs = (INPUT_RECORD * 16)()
        n = wintypes.DWORD(0)
        t_end = time.time() + 20
        TYPE_NAMES = {1: "key", 2: "mouse", 4: "window_buffer_size", 8: "menu", 16: "focus"}
        while time.time() < t_end:
            wait = k32.WaitForSingleObject(hin, 200)
            if wait != 0:
                continue
            if not k32.ReadConsoleInputW(hin, recs, 16, ctypes.byref(n)):
                f["read_error"] = ctypes.get_last_error()
                break
            for i in range(n.value):
                r = recs[i]
                tn = TYPE_NAMES.get(r.EventType, "type_%d" % r.EventType)
                f["counts_by_type"][tn] = f["counts_by_type"].get(tn, 0) + 1
                if len(f["events"]) >= 600:
                    continue
                if r.EventType == 1:
                    kk = r.Event.KeyEvent
                    f["events"].append({"t": "key", "down": bool(kk.bKeyDown), "vk": kk.wVirtualKeyCode,
                                        "scan": kk.wVirtualScanCode, "char_code": kk.uChar,
                                        "char": (chr(kk.uChar) if 32 <= kk.uChar < 0xE000 else None),
                                        "ctrl": kk.dwControlKeyState, "repeat": kk.wRepeatCount})
                elif r.EventType == 2:
                    mm = r.Event.MouseEvent
                    f["events"].append({"t": "mouse", "x": mm.dwMousePosition.X, "y": mm.dwMousePosition.Y,
                                        "buttons": mm.dwButtonState, "flags": mm.dwEventFlags,
                                        "ctrl": mm.dwControlKeyState})
                elif r.EventType == 4:
                    ww = r.Event.WindowBufferSizeEvent
                    f["events"].append({"t": "resize", "w": ww.dwSize.X, "h": ww.dwSize.Y})
                else:
                    f["events"].append({"t": tn})
        if ok_i2:
            k32.SetConsoleMode(hin, mi2)
        f["key_events"] = sum(1 for e in f["events"] if e["t"] == "key")
        f["mouse_events"] = sum(1 for e in f["events"] if e["t"] == "mouse")
        f["resize_events"] = sum(1 for e in f["events"] if e["t"] == "resize")
        f["distinct_vk"] = sorted(set(e["vk"] for e in f["events"] if e["t"] == "key"))
        f["mouse_button_states_seen"] = sorted(set(e["buttons"] for e in f["events"] if e["t"] == "mouse"))
        f["mouse_event_flags_seen"] = sorted(set(e["flags"] for e in f["events"] if e["t"] == "mouse"))
    except Exception as exc:
        f["error"] = repr(exc)
    RESULT["input_probe"] = f
    out("  klawisze=%s  mysz=%s  resize=%s  VK=%s"
        % (f.get("key_events"), f.get("mouse_events"), f.get("resize_events"), f.get("distinct_vk")))
    out("  przyciski myszy (maski)=%s  flags=%s"
        % (f.get("mouse_button_states_seen"), f.get("mouse_event_flags_seen")))

    # ---------------- fonty z rejestru ----------------
    fonts = {}
    try:
        import winreg
        base = "Console"
        sub = []
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base) as k:
                i = 0
                while True:
                    try:
                        sub.append(winreg.EnumKey(k, i))
                        i += 1
                    except OSError:
                        break
        except OSError as exc:
            fonts["open_error"] = repr(exc)
        fonts["subkeys"] = sub[:60]

        def read_key(path):
            d = {}
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as k:
                    for vn in ("FaceName", "FontSize", "FontFamily", "ScreenColors", "PopupColors",
                               "CursorSize", "VirtualTerminalLevel", "ForceV2", "ScreenBufferSize",
                               "WindowSize", "CodePage"):
                        try:
                            d[vn] = winreg.QueryValueEx(k, vn)[0]
                        except OSError:
                            d[vn] = None
            except OSError as exc:
                d["error"] = repr(exc)
            return d

        fonts["default"] = read_key(base)
        for s in sub:
            if "cmd" in s.lower() or "powershell" in s.lower() or "conhost" in s.lower():
                fonts[s] = read_key(base + "\\" + s)
                if len(fonts) > 8:
                    break
    except Exception as exc:
        fonts["error"] = repr(exc)
    RESULT["console_registry"] = fonts
    out("=== Rejestr konsoli ===")
    out("  %s" % json.dumps(fonts.get("default", {}), ensure_ascii=False)[:300])

    # ---------------- werdykt ----------------
    verdict = {
        "vt_supported": RESULT["ansi_probe"]["vt_supported_empirical"],
        "ansi_alt_screen_reliable": False,
        "win32_cell_api_works": bool(RESULT["unicode_probe"]["write_ok"] and RESULT["ansi_probe"]["read_chars_ok"]),
        "unicode_box_drawing_ok": RESULT["unicode_probe"]["roundtrip_identical"],
        "mouse_input_ok": (f.get("mouse_events") or 0) > 0,
        "keyboard_input_ok": (f.get("key_events") or 0) > 0,
        "alt_buffer_ok": bool(d.get("set_active_ok")),
        "full_screen_write_ms": perf.get("ms_per_full_screen_write"),
        "recommended_tui_backend": None,
    }
    if verdict["vt_supported"]:
        verdict["recommended_tui_backend"] = "textual/rich zywo (VT dostepne)"
        verdict["ansi_alt_screen_reliable"] = True
    elif verdict["win32_cell_api_works"]:
        verdict["recommended_tui_backend"] = ("wlasny backend Win32 Console API (WriteConsoleOutputW + "
                                              "ReadConsoleInputW) + rich jako silnik formatowania do bufora")
    else:
        verdict["recommended_tui_backend"] = "zewntrzny emulator terminala (ConEmu/mintty) - wymagane zewnetrzne dane"
    RESULT["verdict"] = verdict
    out("=== WERDYKT ===")
    out("  %s" % json.dumps(verdict, indent=1, ensure_ascii=False))

    dump()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        import traceback
        traceback.print_exc()
        try:
            RESULT["fatal_exception"] = traceback.format_exc()
            dump()
        except Exception:
            pass
        sys.exit(1)
