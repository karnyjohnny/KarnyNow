# -*- coding: utf-8 -*-
"""
T4 v3 harness - D3D9 presentation probe Z IZOLACJA CRASHA.

Historia awarii (wazna, bo wyjasnia dlaczego harness jest taki dziwny):
  v1 zakonczyl sie 0xC0000005 (ACCESS_VIOLATION) na x64 I x86, bez zadnego wyjscia
  (run_rc=3221225477, probe=null, stderr pusty). Powod: caly JSON byl buforowany
  i zapisywany dopiero na koncu, wiec crash zabrał ze soba wszystkie dane.
  Najbardziej prawdopodobny winny: StretchRect lub UpdateSurface na powierzchni
  NV12 wywolane WBREW temu, co mowi CheckDeviceFormatConversion, na starym
  sterowniku Intela 8.15.10.2869.

Co robi v3:
  1. Kazdy ryzykowny etap leci w OSOBNYM PROCESIE. Kod powrotu 0xC0000005
     wskazuje DOKLADNIE, ktora operacja zabija driver - a wszystkie wczesniejsze
     etapy mamy zapisane.
  2. Program C zapisuje plik PRZYROSTOWO i uszczelnia go po kazdym etapie, wiec
     plik na dysku jest poprawnym JSON-em az do miejsca crasha.
  3. Przed StretchRect na NV12 program PYTA sterownika
     (CheckDeviceFormatConversion) i odmawia wywolania, jesli odpowiedz brzmi
     "nie". To jest ta sama zasada, ktora musi trafic do klienta produkcyjnego.

Uruchomienie:
    python T4_d3d9_present.py                # x64 i x86, wszystkie etapy
    python T4_d3d9_present.py --arch x64
    python T4_d3d9_present.py --quick        # mniej iteracji pomiarowych

Wynik: T4_wynik.json (scalony) + T4_skrot.txt (maly)
"""

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
C_SRC = os.path.join(HERE, "T4_d3d9_present.c")
OUT_JSON = os.path.join(HERE, "T4_wynik.json")
OUT_TXT = os.path.join(HERE, "T4_skrot.txt")
TMP = os.path.join(HERE, "T4_tmp")

CFLAGS = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
          "-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601", "-DNTDDI_VERSION=0x06010000",
          "-march=core2", "-msse3", "-static", "-static-libgcc"]
LIBS = ["-ld3d9", "-lgdi32", "-luser32"]
ARCH_CC = {"x64": ["x86_64-w64-mingw32-gcc", "gcc"], "x86": ["i686-w64-mingw32-gcc"]}

# Etapy w kolejnosci OD NAJBEZPIECZNIEJSZEGO. To nie jest kosmetyka: jesli
# driver crashuje, chcemy miec wczesniejsze wyniki i wiedziec, kto zawinil.
STAGES = [
    ("info",       ["--stage=info"]),
    ("formats",    ["--stage=formats"]),
    ("device",     ["--stage=device"]),
    ("surfaces",   ["--stage=surfaces"]),
    # updatesurf CRASHOWAL w v3 (0xC0000005, x64 i x86) -> rozbity na 6 podkrokow.
    # Kazdy podkrok w osobnym procesie: --case=N. Zobacz T4_d3d9_present.c.
    ("updatesurf_sub0_create_sysmem",  ["--stage=updatesurf", "--case=0"]),
    ("updatesurf_sub1_create_default", ["--stage=updatesurf", "--case=1"]),
    ("updatesurf_sub2_lockrect",       ["--stage=updatesurf", "--case=2"]),
    ("updatesurf_sub3_fill",           ["--stage=updatesurf", "--case=3"]),
    ("updatesurf_sub4_unlock",         ["--stage=updatesurf", "--case=4"]),
    ("updatesurf_sub5_updatesurface",  ["--stage=updatesurf", "--case=5"]),
    ("stretch_rgb",         ["--stage=stretch", "--case=0"]),
    ("stretch_yuy2",        ["--stage=stretch", "--case=1"]),
    ("stretch_nv12",        ["--stage=stretch", "--case=2"]),
    ("stretch_nv12_restr",  ["--stage=stretch", "--case=3"]),
    ("stretch_nv12_to_rgb", ["--stage=stretch", "--case=4"]),
    ("bench_rgb",    ["--stage=bench", "--which=0"]),
    ("bench_present",["--stage=bench", "--which=1"]),
    ("bench_nv12",   ["--stage=bench", "--which=2"]),
    ("bench_gdi",    ["--stage=bench", "--which=3"]),
]

CRASH_CODES = {
    3221225477: "0xC0000005 ACCESS_VIOLATION",
    3221225478: "0xC0000006 IN_PAGE_ERROR",
    3221226077: "0xC0000409 STACK_BUFFER_OVERRUN",
    3221225781: "0xC0000135 DLL_NOT_FOUND",
    3221225786: "0xC000013A CTRL_C_EXIT",
    3735928559: "0xDEADBEEF",
}


def log(m):
    sys.stdout.write(m + "\n")
    sys.stdout.flush()


def run(cmd, timeout=300):
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             universal_newlines=True, errors="replace")
        o, e = p.communicate()
        return p.returncode, o or "", e or ""
    except Exception as exc:
        return 127, "", repr(exc)


def pick_cc(cands):
    for c in cands:
        if shutil.which(c):
            return c
    return None


def describe_rc(rc):
    if rc == 0:
        return "ok"
    u = rc & 0xFFFFFFFF
    return CRASH_CODES.get(u, CRASH_CODES.get(rc, "rc=%d (0x%08X)" % (rc, u)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", default="both", choices=["x64", "x86", "both"])
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", default=None, help="tylko te etapy, po przecinku")
    args = ap.parse_args()

    os.makedirs(TMP, exist_ok=True)
    iters = "40" if args.quick else "200"

    report = {
        "test": "T4_d3d9_present_v3_harness",
        "when_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "python": sys.version,
        "platform": platform.platform(),
        "iters": iters,
        "archs": {},
    }

    if not os.path.exists(C_SRC):
        log("BLAD: brak %s" % C_SRC)
        return 2

    arches = ["x64", "x86"] if args.arch == "both" else [args.arch]
    wanted = set(s[0] for s in STAGES)
    if args.only:
        wanted = set(x.strip() for x in args.only.split(",") if x.strip())

    for arch in arches:
        cc = pick_cc(ARCH_CC[arch])
        if not cc:
            report["archs"][arch] = {"error": "no compiler"}
            log("[%s] brak kompilatora" % arch)
            continue
        exe = os.path.join(TMP, "t4_%s.exe" % arch)
        cmd = [cc] + CFLAGS + ["-o", exe, C_SRC] + LIBS
        rc, o, e = run(cmd, timeout=600)
        entry = {"cc": cc, "compile_rc": rc, "compile_cmd": cmd,
                 "compile_warnings": e.strip()[-3000:], "stages": {}}
        log("[%s] compile rc=%s" % (arch, rc))
        if rc != 0:
            log("    %s" % e.strip()[-1500:])
            report["archs"][arch] = entry
            continue
        if not os.path.exists(exe):
            entry["error"] = "exe missing"
            report["archs"][arch] = entry
            continue

        for (name, extra) in STAGES:
            if name not in wanted:
                continue
            stage_dir = os.path.join(TMP, "stages")
            os.makedirs(stage_dir, exist_ok=True)
            outp = os.path.join(stage_dir, "t4_%s_%s.json" % (arch, name))
            # NIE usuwamy poprzedniej wersji: przy debugowaniu wlasnie te pliki
            # sa najcenniejsze (harness v3 je kasowal i przez to nie mielismy
            # czego obejrzec po crashu).
            full = [exe, "--out=" + outp] + extra + ["--iters=" + iters]
            log("    [%s] %-22s ..." % (arch, name))
            rcr, outr, errr = run(full, timeout=900)
            parsed = None
            if os.path.exists(outp):
                try:
                    with open(outp, "r", encoding="utf-8", errors="replace") as fh:
                        parsed = json.load(fh)
                except ValueError as exc:
                    head = ""
                    try:
                        with open(outp, "r", encoding="utf-8", errors="replace") as fh:
                            head = fh.read()
                    except OSError as exc2:
                        head = "<read error %r>" % (exc2,)
                    parsed = {"json_parse_error": repr(exc),
                              "file_bytes": len(head),
                              "partial_head": head[:4000]}
            st = {
                "run_rc": rcr,
                "run_status": describe_rc(rcr),
                "crashed": rcr != 0,
                "stderr": errr.strip()[-1500:],
                "stages_completed": (parsed or {}).get("stages") and
                                    [x.get("stage") for x in parsed["stages"] if isinstance(x, dict)],
                "data": parsed,
            }
            entry["stages"][name] = st
            if isinstance(parsed, dict) and parsed.get("json_parse_error"):
                log("        !! plik nie jest poprawnym JSON-em (%s bajtow): %s"
                    % (parsed.get("file_bytes"), (parsed.get("partial_head") or "")[:220]))
            if rcr == 0:
                log("        ok  (%d etapow w pliku)" % (len(st["stages_completed"] or [])))
            else:
                log("        CRASH/FAIL: %s" % st["run_status"])
                if st["stages_completed"]:
                    log("        ostatni ZAPISANY etap: %s" % st["stages_completed"][-1])
                if errr.strip():
                    log("        stderr: %s" % errr.strip()[-300:])
        report["archs"][arch] = entry

    # ---------------- analiza ----------------
    analysis = {"crashes": [], "nv12_verdict": None, "recommended_present_path": None,
                "refresh_hz": None, "mode_60hz_available": None, "notes": []}
    for arch, entry in report["archs"].items():
        for name, st in (entry.get("stages") or {}).items():
            if st.get("crashed"):
                analysis["crashes"].append({"arch": arch, "stage": name,
                                            "status": st.get("run_status"),
                                            "last_saved_stage": (st.get("stages_completed") or [None])[-1]})
        # info: odswiezanie
        info = ((entry.get("stages") or {}).get("info") or {}).get("data") or {}
        for stg in info.get("stages", []):
            if isinstance(stg, dict) and stg.get("stage") == "info":
                cdm = stg.get("current_display_mode") or {}
                if cdm.get("refresh_hz"):
                    analysis["refresh_hz"] = cdm["refresh_hz"]
                analysis["mode_60hz_available"] = stg.get("mode_1280x800_at_60hz_available")
                analysis["modes_1280x800_max_hz_seen"] = stg.get("modes_1280x800_max_hz_seen")
                analysis["display_mode_d3d"] = stg.get("display_mode")
                caps = stg.get("caps_hal") or {}
                analysis["caps"] = {"stretch_rect_filter_caps": caps.get("stretch_rect_filter_caps"),
                                    "srf_linear": caps.get("srf_linear"),
                                    "srf_point": caps.get("srf_point"),
                                    "max_texture": [caps.get("max_texture_width"), caps.get("max_texture_height")],
                                    "present_interval_immediate": caps.get("present_interval_immediate")}
        # stretch: co przeszlo
        for nm in ("stretch_rgb", "stretch_yuy2", "stretch_nv12", "stretch_nv12_restr", "stretch_nv12_to_rgb"):
            st = (entry.get("stages") or {}).get(nm)
            if not st:
                continue
            data = st.get("data") or {}
            for stg in data.get("stages", []):
                if isinstance(stg, dict) and stg.get("stage") == "stretch":
                    analysis.setdefault("stretch_results", []).append(
                        {"arch": arch, "case": stg.get("case"),
                         "check_format_conversion": stg.get("check_format_conversion"),
                         "conversion_allowed": stg.get("conversion_allowed"),
                         "linear_ok": stg.get("linear_ok"), "linear": stg.get("linear"),
                         "point_ok": stg.get("point_ok"),
                         "skipped": stg.get("skipped"),
                         "crashed": st.get("crashed")})
        # bench
        for nm in ("bench_rgb", "bench_present", "bench_nv12", "bench_gdi"):
            st = (entry.get("stages") or {}).get(nm)
            if not st:
                continue
            data = st.get("data") or {}
            for stg in data.get("stages", []):
                if isinstance(stg, dict) and stg.get("stage") == "bench":
                    t = stg.get("timing") or {}
                    if t:
                        analysis.setdefault("bench", []).append(
                            {"arch": arch, "bench": stg.get("bench"),
                             "ms_mean": t.get("ms_mean"), "ms_median": t.get("ms_median"),
                             "ms_p95": t.get("ms_p95"), "fps_at_mean": t.get("fps_at_mean")})

    # werdykt sciezki prezentacji
    def case_ok(name):
        for r in analysis.get("stretch_results", []):
            if r.get("case") == name and r.get("linear_ok"):
                return True
        return False

    if case_ok("nv12_to_backbuffer_scaled"):
        analysis["nv12_verdict"] = "NV12 StretchRect DZIALA"
        analysis["recommended_present_path"] = ("D3D9 NV12 StretchRect: dekoder pisze NV12 do "
                                                "powierzchni DEFAULT (albo SYSTEMMEM + UpdateSurface), "
                                                "GPU robi CSC i skalowanie. CPU nie dotyka pikseli.")
    elif case_ok("yuy2_to_backbuffer_scaled"):
        analysis["nv12_verdict"] = "NV12 StretchRect NIE działa, YUY2 działa"
        analysis["recommended_present_path"] = ("D3D9 YUY2 StretchRect: konwersja NV12->YUY2 na CPU "
                                                "(~0,05 cyklu/piksel = ~0,7% budżetu przy 720p30), "
                                                "skalowanie i prezentacja na GPU.")
    elif case_ok("x8r8g8b8_to_backbuffer_scaled"):
        analysis["nv12_verdict"] = "tylko RGB StretchRect działa"
        analysis["recommended_present_path"] = ("D3D9 X8R8G8B8 StretchRect: NV12->RGB na CPU "
                                                "(drogo, ~20-25% według T1) albo GDI. Rozważyć "
                                                "prezentację 1:1 bez skalowania.")
    else:
        analysis["nv12_verdict"] = "żaden StretchRect nie potwierdzony"
        analysis["recommended_present_path"] = ("GDI SetDIBitsToDevice (patrz bench_gdi) - ostateczność; "
                                                "albo D3D9 texture + UpdateRect z NV12->RGB na CPU.")

    if analysis["crashes"]:
        analysis["notes"].append(
            "CRASH wykryty w: %s. To jest INFORMACJA, nie porażka testu - dokładnie o to "
            "chodziło w izolacji etapów. Klient produkcyjny musi mieć tę samą zasadę: "
            "CheckDeviceFormatConversion PRZED StretchRect, a przy pierwszej awarii "
            "trwałe przejście na następny fallback (łańcuch NV12 -> YUY2 -> RGB -> GDI)."
            % ", ".join("%s/%s" % (c["arch"], c["stage"]) for c in analysis["crashes"]))
    report["analysis"] = analysis

    # ---------------- zapis ----------------
    with open(OUT_JSON, "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=1)

    with open(OUT_TXT, "w", encoding="utf-8") as fh:
        fh.write("T4 v3 SKROT (maly plik - jesli JSON za duzy, przeslij TEN)\n\n")
        fh.write("refresh_hz=%s  mode_60hz_available=%s  max_hz_seen=%s\n"
                 % (analysis.get("refresh_hz"), analysis.get("mode_60hz_available"),
                    analysis.get("modes_1280x800_max_hz_seen")))
        fh.write("d3d display_mode=%s\n" % json.dumps(analysis.get("display_mode_d3d")))
        fh.write("caps=%s\n\n" % json.dumps(analysis.get("caps")))
        fh.write("CRASHE:\n")
        if analysis["crashes"]:
            for c in analysis["crashes"]:
                fh.write("  %s/%s -> %s (ostatni zapisany etap: %s)\n"
                         % (c["arch"], c["stage"], c["status"], c["last_saved_stage"]))
        else:
            fh.write("  brak\n")
        fh.write("\nStretchRect:\n")
        for r in analysis.get("stretch_results", []):
            fh.write("  %-6s %-32s conv_allowed=%s linear_ok=%s point_ok=%s hr=%s skipped=%s crashed=%s\n"
                     % (r["arch"], r["case"], r["conversion_allowed"], r["linear_ok"],
                        r["point_ok"], r["linear"], r["skipped"], r["crashed"]))
        fh.write("\nBenchmarki:\n")
        for b in analysis.get("bench", []):
            fh.write("  %-6s %-22s mean=%s ms median=%s p95=%s -> %s fps\n"
                     % (b["arch"], b["bench"], b["ms_mean"], b["ms_median"], b["ms_p95"], b["fps_at_mean"]))
        fh.write("\nWERDYKT: %s\n" % analysis.get("nv12_verdict"))
        fh.write("Sciezka prezentacji: %s\n" % analysis.get("recommended_present_path"))
        fh.write("\nUwagi:\n")
        for n in analysis.get("notes", []):
            fh.write("  - %s\n" % n)

    log("")
    log("=== ANALIZA T4 v3 ===")
    log("  refresh_hz = %s   60Hz dostepne = %s" % (analysis.get("refresh_hz"),
                                                    analysis.get("mode_60hz_available")))
    if analysis["crashes"]:
        log("  CRASHE:")
        for c in analysis["crashes"]:
            log("    %s/%s -> %s (ostatni zapisany: %s)"
                % (c["arch"], c["stage"], c["status"], c["last_saved_stage"]))
    else:
        log("  crashy: brak")
    log("  StretchRect:")
    for r in analysis.get("stretch_results", []):
        log("    %-6s %-32s conv_allowed=%s linear_ok=%s crashed=%s"
            % (r["arch"], r["case"], r["conversion_allowed"], r["linear_ok"], r["crashed"]))
    log("  Benchmarki:")
    for b in analysis.get("bench", []):
        log("    %-6s %-22s mean=%s ms -> %s fps" % (b["arch"], b["bench"], b["ms_mean"], b["fps_at_mean"]))
    log("")
    log("  WERDYKT: %s" % analysis.get("nv12_verdict"))
    log("  Sciezka: %s" % analysis.get("recommended_present_path"))
    log("")
    log("Wynik: %s" % OUT_JSON)
    log("Skrot: %s  (%d bajtow)" % (OUT_TXT, os.path.getsize(OUT_TXT)))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
