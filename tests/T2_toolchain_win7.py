# -*- coding: utf-8 -*-
"""
T2 - Czy toolchain mingw GCC 16.2.0 produkuje binarki, ktore DZIALAJA na Windows 7?

To jest bramka krytyczna: nowoczesne GCC/mingw-w64 domyslnie celuje w nowsze
Windows (_WIN32_WINNT / WINVER), a CRT moze importowac API, ktorych na Win7 nie ma.
Jesli ten test nie przejdzie, caly projekt w C staje, dopoki nie znajdziemy
starszego toolchaina.

Test robi trzy rzeczy:
  1. Kompiluje programik diagnostyczny w roznych wariantach flag (x64 i x86).
  2. Uruchamia kazdy wariant i zbiera, co sam o sobie zamelduje.
  3. Parsuje naglowek PE i liste importow DLL wynikowej binarki.

Wymagania: gcc na PATH (x86_64-w64-mingw32-gcc i/lub i686-w64-mingw32-gcc).
Czas: ok. 1-3 minuty.

Uruchomienie:
    python T2_toolchain_win7.py
Wynik: T2_wynik.json  <- przeslij mi ten plik.
"""

import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_JSON = os.path.join(HERE, "T2_wynik.json")
TMP = os.path.join(HERE, "T2_tmp")

C_SOURCE = r"""
/* T2 probe: what does this toolchain target, and what does it link against? */
#include <stdio.h>
#include <windows.h>
#include <d3d9.h>
#include <winsock2.h>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT_REPORTED -1
#else
#define _WIN32_WINNT_REPORTED _WIN32_WINNT
#endif
#ifndef WINVER
#define WINVER_REPORTED -1
#else
#define WINVER_REPORTED WINVER
#endif
#ifndef NTDDI_VERSION
#define NTDDI_REPORTED -1
#else
#define NTDDI_REPORTED NTDDI_VERSION
#endif
#ifndef __MINGW64_VERSION_MAJOR
#define MINGW64_MAJOR -1
#else
#define MINGW64_MAJOR __MINGW64_VERSION_MAJOR
#endif
#ifndef __MINGW64_VERSION_MINOR
#define MINGW64_MINOR -1
#else
#define MINGW64_MINOR __MINGW64_VERSION_MINOR
#endif

int main(void) {
    OSVERSIONINFOW ovi;
    IDirect3D9 *d3d;
    D3DADAPTER_IDENTIFIER9 ident;
    HRESULT hr;
    unsigned int w, h;

    printf("compiled_win32_winnt=0x%04X\n", (unsigned)_WIN32_WINNT_REPORTED);
    printf("compiled_winver=0x%04X\n", (unsigned)WINVER_REPORTED);
    printf("compiled_ntddi=0x%08X\n", (unsigned)NTDDI_REPORTED);
    printf("compiled_mingw64=%d.%d\n", MINGW64_MAJOR, MINGW64_MINOR);
    printf("compiled_pointer_bits=%d\n", (int)(sizeof(void*) * 8));
#ifdef __GNUC__
    printf("compiled_gcc=%d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
#ifdef __SSE3__
    printf("compiled_sse3=1\n");
#else
    printf("compiled_sse3=0\n");
#endif
#ifdef __SSSE3__
    printf("compiled_ssse3=1\n");
#else
    printf("compiled_ssse3=0\n");
#endif
#ifdef __AVX__
    printf("compiled_avx=1\n");
#else
    printf("compiled_avx=0\n");
#endif

    ZeroMemory(&ovi, sizeof(ovi));
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    /* GetVersionEx jest manifest-gated od Win8.1; na Win7 zwraca prawde. */
    if (GetVersionExW(&ovi)) {
        printf("runtime_os=%lu.%lu.%lu\n", ovi.dwMajorVersion, ovi.dwMinorVersion, ovi.dwBuildNumber);
    } else {
        printf("runtime_os=GetVersionEx_failed(%ld)\n", (long)GetLastError());
    }

    /* Czy d3d9 w ogole sie linkuje i tworzy urzadzenie? */
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    printf("d3d9_create=%s\n", d3d ? "ok" : "NULL");
    if (d3d) {
        if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &ident))) {
            printf("d3d9_desc=%s\n", ident.Description);
            printf("d3d9_driver=%s\n", ident.Driver);
            printf("d3d9_vendor_device=%04X:%04X\n", ident.VendorId, ident.DeviceId);
        }
        IDirect3D9_Release(d3d);
    }

    /* Czy winsock2 dziala (potrzebny dla UDP Mjolnir)? */
    {
        WSADATA wsa;
        int r = WSAStartup(MAKEWORD(2, 2), &wsa);
        printf("wsastartup=%d\n", r);
        if (r == 0) WSACleanup();
    }

    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    printf("screen=%ux%u\n", w, h);
    printf("PROBE_OK\n");
    return 0;
}
"""

VARIANTS = [
    ("default", []),
    ("win7", ["-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601", "-DNTDDI_VERSION=0x06010000"]),
    ("win7_static", ["-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601", "-DNTDDI_VERSION=0x06010000",
                     "-static", "-static-libgcc"]),
    ("win7_core2", ["-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601",
                    "-march=core2", "-msse3", "-O2", "-static", "-static-libgcc"]),
    ("win7_subsys601", ["-D_WIN32_WINNT=0x0601", "-DWINVER=0x0601", "-static", "-static-libgcc",
                        "-Wl,--major-subsystem-version,6.01", "-Wl,--minor-subsystem-version,01",
                        "-Wl,--major-os-version,6.01", "-Wl,--minor-os-version,01"]),
]

# DLL-e, ktore sa obecne na Windows 7 SP1 x64. Wszystko poza ta lista = czerwona flaga.
KNOWN_WIN7_DLLS = set("""
advapi32.dll bcrypt.dll cabinet.dll cfgmgr32.dll comctl32.dll comdlg32.dll credui.dll
crypt32.dll cryptui.dll d2d1.dll d3d9.dll d3d10.dll d3d10_1.dll d3d11.dll dbus-1.dll
dbghelp.dll dhcpcsvc.dll dinput8.dll ddraw.dll ddrawex.dll dnsapi.dll dsound.dll
dwmapi.dll dwrite.dll dxgi.dll dxva2.dll gdi32.dll gdiplus.dll iertutil.dll imagehlp.dll
imm32.dll iphlpapi.dll kernel32.dll loadperf.dll mf.dll mfplat.dll mfreadwrite.dll
mpr.dll msacm32.dll msvcrt.dll msimg32.dll mswsock.dll ncrypt.dll netapi32.dll
normaliz.dll ntdll.dll ntoskrnl.exe ole32.dll oleacc.dll oleaut32.dll opengl32.dll
pdh.dll powrprof.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll shell32.dll
shfolder.dll shlwapi.dll slc.dll snmpapi.dll urlmon.dll user32.dll userenv.dll
usp10.dll uxtheme.dll version.dll vfw32.dll wbemuuid.dll winhttp.dll wininet.dll
winmm.dll winscard.dll winspool.drv wintrust.dll wkscli.dll wldap32.dll wmfhotfix.dll
wtsapi32.dll xinput1_3.dll xinput9_1_0.dll ws2_32.dll wsock32.dll winsock2.dll
propsys.dll d3dcompiler_43.dll d3dx9_43.dll msvcp140.dll vcruntime140.dll
api-ms-win-core-*.dll ucrtbase.dll
""".split())

# API, ktorych NIE ma na Windows 7 (proba uzycia = binarka nie wystartuje).
NOT_ON_WIN7 = [
    "GetSystemTimePreciseAsFileTime",     # Win8+
    "InitializeConditionVariable",        # Vista+ (ok) - zostawiamy jako informacja
    "TryAcquireSRWLockExclusive",         # Win7+ (ok)
    "SetThreadDescription",               # Win10 1607+
    "GetThreadDescription",               # Win10 1607+
    "SetDefaultDllDirectories",           # KB2533623 (Win7 po poprawce)
    "AddDllDirectory",                    # KB2533623
    "GetOverlappedResultEx",              # Win8+
    "PrefetchVirtualMemory",              # Win8+
    "QueryUnbiasedInterruptTimePrecise",  # Win10+
    "WaitOnAddress",                      # Win8+
    "WakeByAddressSingle",                # Win8+
    "RoInitialize",                       # Win8+ (WinRT)
    "CreateProcess2",                     # Win10+
    "GetProcessInformation",              # Win8.1+
    "SetProcessInformation",              # Win8.1+
    "GetSystemTimePreciseAsFileTime",
    "RaiseFailFastException",             # Win7+ (ok)
    "BCryptGenRandom",                    # Vista+ (ok)
    "PathCchCanonicalizeEx",              # Win8+ (api-ms-win-core-path)
]
HARD_MISSING_ON_WIN7 = {
    "GetSystemTimePreciseAsFileTime", "SetThreadDescription", "GetThreadDescription",
    "GetOverlappedResultEx", "PrefetchVirtualMemory", "QueryUnbiasedInterruptTimePrecise",
    "WaitOnAddress", "WakeByAddressSingle", "WakeByAddressAll", "RoInitialize",
    "CreateProcess2", "GetProcessInformation", "SetProcessInformation",
    "PathCchCanonicalizeEx", "SetDefaultDllDirectories", "AddDllDirectory",
    "RemoveDllDirectory", "SetThreadSelectedExecutionStateM2",
    "GetSystemTimePreciseAsLargeInteger", "InitializeSListHead",
}


def run(cmd, timeout=120):
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             universal_newlines=True, errors="replace")
        out, err = p.communicate()
        return p.returncode, out or "", err or ""
    except Exception as exc:
        return 127, "", repr(exc)


def parse_pe(path):
    """Minimalny parser PE: wersje OS/subsystem z OptionalHeader + lista importowanych DLL."""
    info = {"file": os.path.basename(path), "size": os.path.getsize(path)}
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        info["error"] = repr(exc)
        return info
    if data[:2] != b"MZ":
        info["error"] = "not MZ"
        return info
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        info["error"] = "not PE"
        return info
    coff = pe_off + 4
    machine, nsec, _ts, _psym, _nsym, opt_size, _chars = struct.unpack_from("<HHIIIHH", data, coff)
    info["machine"] = hex(machine)
    info["machine_name"] = {0x8664: "x86_64", 0x14c: "i386"}.get(machine, "?")
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    pe32plus = (magic == 0x20B)
    # OptionalHeader (PE32/PE32+):
    #  off 40 (PE32) / 40 (PE32+) : MajorOperatingSystemVersion (2), MinorOperatingSystemVersion (2)
    #  off 48 : MajorSubsystemVersion, MinorSubsystemVersion
    base = opt + 40
    maj_os, min_os, maj_img, min_img = struct.unpack_from("<HHHH", data, base)
    base2 = opt + 48
    maj_sub, min_sub = struct.unpack_from("<HH", data, base2)
    info["major_os_version"] = "%d.%d" % (maj_os, min_os)
    info["major_subsystem_version"] = "%d.%d" % (maj_sub, min_sub)
    info["pe32plus"] = pe32plus

    # Data directory: Import Table = indeks 1
    dd_off = opt + (112 if pe32plus else 96)
    imp_rva, imp_size = struct.unpack_from("<II", data, dd_off + 1 * 8)

    # tabela sekcji -> mapowanie RVA na offset pliku
    sec_off = opt + opt_size
    sections = []
    for i in range(nsec):
        o = sec_off + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
        sections.append((name, vaddr, vsize, raddr, rsize))

    def rva_to_off(rva):
        best = None
        for (_n, va, vs, ra, rs) in sections:
            if va <= rva:
                span = max(vs, rs)
                if rva < va + span:
                    cand = ra + (rva - va)
                    if best is None or va > best[0]:
                        best = (va, cand)
        return best[1] if best else None

    dlls = []
    if imp_rva:
        off = rva_to_off(imp_rva)
        if off:
            for i in range(512):
                ent = off + i * 20
                if ent + 20 > len(data):
                    break
                _oft, _ts, _fwd, name_rva, _first = struct.unpack_from("<IIIII", data, ent)
                if name_rva == 0:
                    break
                no = rva_to_off(name_rva)
                if not no:
                    break
                end = data.find(b"\0", no)
                dlls.append(data[no:end].decode("latin1", "replace"))
    info["imported_dlls"] = dlls
    info["unknown_dlls"] = sorted(set(d.lower() for d in dlls) - KNOWN_WIN7_DLLS)

    # poszukiwanie ryzykownych API w surowych bajtach (sekcje .rdata/.text)
    found = []
    for api in NOT_ON_WIN7:
        for enc in (api.encode("ascii"),):
            if enc in data:
                found.append(api)
                break
    info["risky_api_strings_present"] = found
    info["hard_blockers_present"] = sorted(set(found) & HARD_MISSING_ON_WIN7)
    return info


def main():
    os.makedirs(TMP, exist_ok=True)
    src = os.path.join(TMP, "probe.c")
    with open(src, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(C_SOURCE)

    report = {
        "test": "T2_toolchain_win7",
        "when_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "python": sys.version,
        "platform": platform.platform(),
        "compilers": {},
        "builds": [],
    }

    candidates = ["gcc", "x86_64-w64-mingw32-gcc", "i686-w64-mingw32-gcc", "cc"]
    for cc in candidates:
        if shutil.which(cc):
            rc, out, err = run([cc, "-dumpmachine"])
            rc2, vout, verr = run([cc, "--version"])
            report["compilers"][cc] = {
                "triplet": (out or err).strip().splitlines()[:1],
                "version": (vout or verr).strip().splitlines()[:1],
            }
    if not report["compilers"]:
        report["fatal"] = "no compiler found on PATH"
        with open(OUT_JSON, "w") as fh:
            json.dump(report, fh, indent=1)
        print("BLAD: brak kompilatora na PATH.")
        return 2

    # domyslne makra toolchaina
    for cc in report["compilers"]:
        rc, out, err = run([cc, "-dM", "-E", "-x", "c", os.devnull])
        text = out or ""
        keep = {}
        for key in ("_WIN32_WINNT", "WINVER", "NTDDI_VERSION", "__MINGW64_VERSION_MAJOR",
                    "__MINGW64_VERSION_MINOR", "_WIN64", "__SSE3__", "__SSSE3__", "__AVX__",
                    "__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4"):
            m = re.search(r"^#define\s+%s\s+(.+)$" % re.escape(key), text, re.M)
            if m:
                keep[key] = m.group(1).strip()
        report["compilers"][cc]["default_macros"] = keep

    print("=== Kompilatory ===")
    for cc, meta in report["compilers"].items():
        print("  %s  %s  %s" % (cc, meta["triplet"], meta["default_macros"].get("_WIN32_WINNT", "(undef)")))
    print("")

    for cc in ("gcc", "x86_64-w64-mingw32-gcc", "i686-w64-mingw32-gcc"):
        if cc not in report["compilers"]:
            continue
        for (name, extra) in VARIANTS:
            exe = os.path.join(TMP, "probe_%s_%s.exe" % (cc.replace("-", "_"), name))
            cmd = [cc, "-std=c11", "-O1", "-Wall", "-o", exe, src,
                   "-ld3d9", "-lws2_32", "-lgdi32", "-luser32", "-lwinmm"] + extra
            rc, out, err = run(cmd)
            entry = {"compiler": cc, "variant": name, "flags": extra,
                     "compile_rc": rc, "compile_stderr": err.strip()[-1500:], "exe": exe}
            if rc == 0 and os.path.exists(exe):
                entry["pe"] = parse_pe(exe)
                rcr, outr, errr = run([exe], timeout=60)
                entry["run_rc"] = rcr
                entry["run_stdout"] = outr.strip()
                entry["run_stderr"] = errr.strip()[-400:]
                kv = {}
                for line in outr.splitlines():
                    if "=" in line:
                        k, v = line.split("=", 1)
                        kv[k.strip()] = v.strip()
                entry["run_kv"] = kv
                entry["ran_ok"] = kv.get("PROBE_OK") == "PROBE_OK"
            else:
                entry["ran_ok"] = False
            report["builds"].append(entry)
            print("  %-26s %-16s compile=%s run=%s pe_os=%s subsys=%s unknown_dlls=%s blockers=%s"
                  % (cc, name, rc, entry.get("run_rc"),
                     entry.get("pe", {}).get("major_os_version"),
                     entry.get("pe", {}).get("major_subsystem_version"),
                     entry.get("pe", {}).get("unknown_dlls"),
                     entry.get("pe", {}).get("hard_blockers_present")))
            if rc != 0:
                print("      stderr: %s" % err.strip()[-300:].replace("\n", " "))

    print("")
    print("Wynik: %s" % OUT_JSON)
    with open(OUT_JSON, "w") as fh:
        json.dump(report, fh, indent=1)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
