# -*- coding: utf-8 -*-
"""
T1 - Pomiar sufitu dekodowania H.264 na Core 2 Duo T7250.

Cel: odpowiedziec na pytanie, czy 2 x Merom @ 2 GHz zdola programowo zdekodowac
strumien H.264 o parametrach, jakie oglada nasz klient NVST, i ile CPU kosztuje
konwersja koloru.

Wymagania: python 3.6+, ffmpeg/ffprobe na PATH.
Czas: ok. 3-8 minut (zalezy od dysku i CPU).

Uruchomienie:
    python T1_decode_bench.py
    python T1_decode_bench.py --quick          # krotszy zestaw
    python T1_decode_bench.py --res 1280x720 --fps 30   # tylko jeden wariant

Wynik: raport na konsoli + pelny JSON w  T1_wynik.json
Przeslij mi caly JSON (i to, co wydrukuje konsola).
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_JSON = os.path.join(HERE, "T1_wynik.json")
TMPDIR = os.path.join(HERE, "T1_tmp")

# (szerokosc, wysokosc, fps, etykieta)
FULL_MATRIX = [
    (640, 360, 30, "360p30"),
    (960, 540, 30, "540p30"),
    (960, 540, 60, "540p60"),
    (1280, 720, 30, "720p30"),
    (1280, 720, 60, "720p60"),
    (1920, 1088, 30, "1088p30"),
]
QUICK_MATRIX = [
    (640, 360, 30, "360p30"),
    (960, 540, 30, "540p30"),
    (1280, 720, 30, "720p30"),
    (1280, 720, 60, "720p60"),
]

# Docelowa koperta strumienia z ABI KarnyNow (h264, 8-bit, 4:2:0).
# Bitrate symulujemy tak, jakby serwer trzymal sie naszego limitu, a nie 75 Mbps.
BITRATE_KBPS = {
    "360p30": 1200,
    "540p30": 2500,
    "540p60": 4000,
    "720p30": 4000,
    "720p60": 6000,
    "1088p30": 8000,
}


def log(msg):
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


def run(cmd, timeout=None):
    """Zwraca (rc, stdout, stderr)."""
    try:
        p = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            errors="replace",
        )
    except OSError as exc:
        return 127, "", "Popen failed: %r" % (exc,)
    out, err = p.communicate()
    return p.returncode, out or "", err or ""


def have(exe):
    return shutil.which(exe) is not None


def memory_snapshot():
    """Proba odczytu wolnej pamieci fizycznej przez wmic (Win7 ma wmic)."""
    rc, out, err = run(["wmic", "OS", "get", "FreePhysicalMemory,TotalVisibleMemorySize", "/format:list"])
    res = {}
    if rc == 0:
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                try:
                    res[k.strip()] = int(v.strip())
                except ValueError:
                    pass
    return res


def ffprobe_json(path):
    rc, out, err = run(["ffprobe", "-v", "error", "-print_format", "json",
                        "-show_format", "-show_streams", path])
    if rc != 0:
        return None
    try:
        return json.loads(out)
    except ValueError:
        return None


def make_source(w, h, fps, label, seconds):
    """Generuje syntetyczny klip H.264 o zadanym bitrate.

    testsrc2 + szum daje ruch i detal - dekodowanie nie jest trywialne
    (w przeciwienstwie do statycznego obrazu, ktory mialby mikroskopijny bitrate).
    """
    os.makedirs(TMPDIR, exist_ok=True)
    path = os.path.join(TMPDIR, "src_%s.h264" % label)
    kbps = BITRATE_KBPS.get(label, 4000)
    frames = int(seconds * fps)
    vf = "testsrc2=size=%dx%d:rate=%d,noise=alls=14:allf=t" % (w, h, fps)
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", vf,
        "-frames:v", str(frames),
        "-c:v", "libx264", "-preset", "veryfast", "-profile:v", "high",
        "-level", "5.1", "-pix_fmt", "yuv420p",
        "-b:v", "%dk" % kbps, "-maxrate", "%dk" % (kbps * 2), "-bufsize", "%dk" % (kbps * 2),
        "-x264-params", "keyint=%d:min-keyint=%d:scenecut=0" % (fps, fps),
        path,
    ]
    t0 = time.time()
    rc, out, err = run(cmd)
    dt = time.time() - t0
    info = ffprobe_json(path) if rc == 0 else None
    size = os.path.getsize(path) if rc == 0 and os.path.exists(path) else 0
    return {
        "label": label, "width": w, "height": h, "fps": fps,
        "target_kbps": kbps, "seconds": seconds, "frames": frames,
        "encode_rc": rc, "encode_seconds": round(dt, 1),
        "file": path, "file_bytes": size,
        "actual_kbps": round(size * 8.0 / 1000.0 / seconds, 1) if seconds and size else None,
        "encode_error": (err.strip()[-600:] if rc != 0 else None),
        "probe_codec": ((info or {}).get("streams") or [{}])[0].get("codec_name"),
        "probe_profile": ((info or {}).get("streams") or [{}])[0].get("profile"),
        "probe_pix_fmt": ((info or {}).get("streams") or [{}])[0].get("pix_fmt"),
        "probe_nb_frames": ((info or {}).get("streams") or [{}])[0].get("nb_frames"),
    }


BENCH_RE = re.compile(r"utime=([0-9.]+)s\s+stime=([0-9.]+)s\s+rtime=([0-9.]+)s")
FPS_RE = re.compile(r"video:[0-9]+kB.*?fps=\s*([0-9.]+)")
SPEED_RE = re.compile(r"speed=\s*([0-9.]+)x")


def decode_once(src, threads, thread_type, extra_vf=None, hwaccel=None):
    """Jedno dekodowanie calego klipu do null. Zwraca metryki."""
    cmd = ["ffmpeg", "-hide_banner", "-benchmark", "-loglevel", "info", "-nostats"]
    if hwaccel:
        cmd += ["-hwaccel", hwaccel]
    cmd += ["-i", src]
    cmd += ["-threads", str(threads)]
    if thread_type:
        cmd += ["-thread_type", thread_type]
    if extra_vf:
        cmd += ["-vf", extra_vf]
    cmd += ["-f", "null", "-"]
    t0 = time.time()
    rc, out, err = run(cmd)
    wall = time.time() - t0
    text = err + "\n" + out
    m = BENCH_RE.search(text)
    utime = stime = rtime = None
    if m:
        utime, stime, rtime = float(m.group(1)), float(m.group(2)), float(m.group(3))
    res_hw = {}
    if hwaccel:
        low = text.lower()
        res_hw["hwaccel_requested"] = hwaccel
        res_hw["fell_back_to_software"] = bool(
            "fallback" in low or "failed to initialize" in low
            or "hardware acceleration" in low or "no hwaccel" in low
            or "cuvid" in low and "error" in low)
        res_hw["hwaccel_lines"] = [ln.strip() for ln in text.splitlines()
                                   if ("hwaccel" in ln.lower() or "dxva" in ln.lower())][:8]
    fps = None
    m2 = FPS_RE.search(text)
    if m2:
        fps = float(m2.group(1))
    speed = None
    m3 = SPEED_RE.search(text)
    if m3:
        speed = float(m3.group(1))
    out = {
        "threads": threads, "thread_type": thread_type,
        "vf": extra_vf, "hwaccel": hwaccel,
        "rc": rc, "wall_seconds": round(wall, 3),
        "utime_s": utime, "stime_s": stime, "rtime_s": rtime,
        "reported_fps": fps, "reported_speed_x": speed,
        "cpu_process_seconds": round((utime or 0) + (stime or 0), 3),
        "error_tail": (text.strip()[-500:] if rc != 0 else None),
    }
    if res_hw:
        out["hwaccel_probe"] = res_hw
    return out


def summarize(res, frames, fps_target):
    """Wyprowadza fps i procent CPU z pomiaru."""
    rt = res.get("rtime_s") or res.get("wall_seconds") or 0
    cpu = res.get("cpu_process_seconds") or 0
    derived_fps = round(frames / rt, 2) if rt else None
    cpu_pct = round(100.0 * cpu / rt, 1) if rt else None
    res["derived_fps"] = derived_fps
    res["cpu_percent_of_one_core_pair"] = cpu_pct
    res["realtime_ok"] = bool(derived_fps is not None and derived_fps >= fps_target * 0.98)
    res["headroom_x"] = round(derived_fps / fps_target, 2) if derived_fps and fps_target else None
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true", help="krotsza macierz testow")
    ap.add_argument("--seconds", type=float, default=8.0, help="dlugosc klipu (domyslnie 8 s)")
    ap.add_argument("--repeats", type=int, default=2, help="ile razy powtorzyc kazdy pomiar")
    ap.add_argument("--res", default=None, help="tylko ta rozdzielczosc, np. 1280x720")
    ap.add_argument("--fps", type=int, default=None, help="tylko to fps")
    ap.add_argument("--skip-hwaccel", action="store_true")
    args = ap.parse_args()

    report = {
        "test": "T1_decode_bench",
        "when_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "python": sys.version,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "ffmpeg": None,
        "memory_before": memory_snapshot(),
        "args": vars(args),
        "sources": [],
        "results": [],
        "verdict": {},
    }

    if not have("ffmpeg") or not have("ffprobe"):
        log("BLAD: ffmpeg/ffprobe nie sa na PATH. Ten test wymaga ffmpeg.")
        report["fatal"] = "ffmpeg not on PATH"
        with open(OUT_JSON, "w") as fh:
            json.dump(report, fh, indent=1)
        return 2

    rc, ver, err = run(["ffmpeg", "-hide_banner", "-version"])
    report["ffmpeg"] = (ver or err).strip().splitlines()[:3]
    log("ffmpeg: %s" % (report["ffmpeg"][0] if report["ffmpeg"] else "?"))
    log("cpu_count=%s  platform=%s" % (report["cpu_count"], report["platform"]))
    log("")

    matrix = QUICK_MATRIX if args.quick else FULL_MATRIX
    if args.res or args.fps:
        want = args.res
        matrix = [m for m in matrix
                  if (want is None or ("%dx%d" % (m[0], m[1])) == want)
                  and (args.fps is None or m[2] == args.fps)]
        if not matrix:
            log("BLAD: --res/--fps nie pasuja do zadnego wariantu.")
            return 2

    # --- generacja zrodel ---
    log("=== Generowanie klipow testowych (%.0f s kazdy) ===" % args.seconds)
    for (w, h, fps, label) in matrix:
        log("  %-9s %dx%d@%d ..." % (label, w, h, fps))
        src = make_source(w, h, fps, label, args.seconds)
        if src["encode_rc"] != 0:
            log("    ENCODE FAILED rc=%s: %s" % (src["encode_rc"], src["encode_error"]))
        else:
            log("    ok  %.1f MB  real=%s kbps  encode %.1f s"
                % (src["file_bytes"] / 1048576.0, src["actual_kbps"], src["encode_seconds"]))
        report["sources"].append(src)
    log("")

    # --- dekodowanie ---
    log("=== Dekodowanie (to jest wlasciwy pomiar) ===")
    configs = [
        (1, None, None, "decode_only_1t"),
        (2, None, None, "decode_only_2t"),
        (2, "frame", None, "decode_only_2t_frame"),
        (2, None, "format=nv12", "to_nv12_2t"),
        (2, None, "format=rgb24", "to_rgb24_2t_swconv"),
        (2, None, "format=yuyv422", "to_yuy2_2t_swconv"),
        (2, None, "scale=1280:800:flags=fast_bilinear,format=nv12", "scale800_nv12_2t"),
    ]
    if not args.skip_hwaccel:
        configs.insert(0, (2, None, None, "dxva2_expected_to_fail"))

    for src in report["sources"]:
        if src["encode_rc"] != 0:
            continue
        for (threads, ttype, vf, name) in configs:
            if name == "dxva2_expected_to_fail":
                best = decode_once(src["file"], threads, ttype, vf, hwaccel="dxva2")
            else:
                runs = [decode_once(src["file"], threads, ttype, vf) for _ in range(max(1, args.repeats))]
                ok = [r for r in runs if r["rc"] == 0]
                best = min(ok, key=lambda r: r["wall_seconds"]) if ok else runs[0]
                best["repeats"] = len(runs)
                # celowo NIE zapisujemy all_runs: przy 6 zrodla x 8 konfiguracji
                # JSON rosnie do ~150 KB i kopiowany plik zostaje uciety.
                best["all_runs_wall_seconds"] = [r["wall_seconds"] for r in runs]
                best["all_runs_fps"] = [r.get("derived_fps") for r in runs]
            summarize(best, src["frames"], src["fps"])
            best["source_label"] = src["label"]
            best["config"] = name
            report["results"].append(best)
            log("  %-9s %-26s fps=%-7s cpu=%-6s %s%s"
                % (src["label"], name,
                   best.get("derived_fps"), best.get("cpu_process_seconds") is not None
                   and str(best.get("cpu_percent_of_one_core_pair")) + "%" or "?",
                   "REALTIME-OK" if best.get("realtime_ok") else "TOO-SLOW",
                   "  [hwaccel]" if best.get("hwaccel") else ""))
            if best["rc"] != 0:
                log("      rc=%s  %s" % (best["rc"], (best.get("error_tail") or "")[:220].replace("\n", " ")))
    log("")

    report["memory_after"] = memory_snapshot()

    # --- werdykt ---
    verdict = {}
    for label in set(r["source_label"] for r in report["results"]):
        rows = [r for r in report["results"]
                if r["source_label"] == label and r["rc"] == 0 and r.get("derived_fps")]
        if not rows:
            continue
        pure = [r for r in rows if r["config"].startswith("decode_only")]
        sw = [r for r in rows if r["config"].endswith("swconv")]
        verdict[label] = {
            "best_decode_fps": max((r["derived_fps"] for r in pure), default=None),
            "best_decode_cpu_pct": min((r["cpu_percent_of_one_core_pair"] for r in pure), default=None),
            "best_decode_with_sw_rgb_fps": max((r["derived_fps"] for r in sw), default=None),
            "sw_rgb_penalty_pct": None,
        }
        if verdict[label]["best_decode_fps"] and verdict[label]["best_decode_with_sw_rgb_fps"]:
            verdict[label]["sw_rgb_penalty_pct"] = round(
                100.0 * (1.0 - verdict[label]["best_decode_with_sw_rgb_fps"] / verdict[label]["best_decode_fps"]), 1)
    report["verdict"] = verdict

    log("=== Podsumowanie ===")
    for label, v in sorted(verdict.items()):
        log("  %-9s dekod=%s fps (cpu %s%%)   z konwersja RGB na CPU=%s fps   strata=%s%%"
            % (label, v["best_decode_fps"], v["best_decode_cpu_pct"],
               v["best_decode_with_sw_rgb_fps"], v["sw_rgb_penalty_pct"]))
    log("")
    log("Pliku wyniku: %s" % OUT_JSON)
    log("Przeslij mi caly ten JSON.")

    with open(OUT_JSON, "w") as fh:
        json.dump(report, fh, indent=1)

    txt_path = os.path.join(HERE, "T1_skrot.txt")
    with open(txt_path, "w", encoding="utf-8") as fh:
        fh.write("T1 skrot (ten plik jest maly - jesli JSON jest uciety, przeslij TEN)\n")
        fh.write("ffmpeg: %s\n" % (report["ffmpeg"][0] if report["ffmpeg"] else "?"))
        fh.write("mem_before=%s mem_after=%s\n\n" % (report.get("memory_before"), report.get("memory_after")))
        fh.write("%-9s %-28s %4s %9s %9s %8s %7s %s\n"
                 % ("src", "config", "rc", "d_fps", "speed_x", "cpu_s", "cpu%", "flag"))
        for r in report["results"]:
            fh.write("%-9s %-28s %4s %9s %9s %8s %7s %s\n"
                     % (r.get("source_label"), r.get("config"), r.get("rc"),
                        r.get("derived_fps"), r.get("reported_speed_x"),
                        r.get("cpu_process_seconds"), r.get("cpu_percent_of_one_core_pair"),
                        "OK" if r.get("realtime_ok") else "SLOW"))
            hp = r.get("hwaccel_probe")
            if hp:
                fh.write("           hwaccel: fallback_to_sw=%s lines=%s\n"
                         % (hp.get("fell_back_to_software"), hp.get("hwaccel_lines")))
        fh.write("\nWERDYKT:\n")
        fh.write(json.dumps(report["verdict"], indent=1))
        fh.write("\n")
    log("Skrot (maly plik): %s" % txt_path)
    log("JSON (pelny): %s  (%d bajtow)" % (OUT_JSON, os.path.getsize(OUT_JSON)))
    log("Jesli JSON jest za duzy do zalaczenia - przeslij T1_skrot.txt.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
