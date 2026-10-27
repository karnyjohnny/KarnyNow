# -*- coding: utf-8 -*-
"""Weryfikator: porownuje liczbe argumentow makr IDirect3D* uzytych w T4
z ich definicjami w prawdziwym d3d9.h mingw-w64. Bez kompilatora."""
import re, sys

HDR = "src/hdr_d3d9.h"
SRC = "tests/T4_d3d9_present.c"

hdr = open(HDR, encoding="utf-8", errors="replace").read()
src = open(SRC, encoding="utf-8", errors="replace").read()

# 1) zbierz definicje makr: #define NAME(p,a,b,...)
macro_args = {}
for m in re.finditer(r'#define\s+(IDirect3D\w+|IDirect3DDevice9\w+|IDirect3DSurface9\w+)\(([^)]*)\)', hdr):
    name, params = m.group(1), m.group(2)
    n = len([p for p in params.split(",") if p.strip()])
    # makro jest zdefiniowane dwa razy (C i C++); liczba argumentow ta sama
    if name in macro_args and macro_args[name] != n:
        print("UWAGA: rozne liczby argumentow dla %s: %d vs %d" % (name, macro_args[name], n))
    macro_args[name] = n

# 2) usun komentarze i stringi ze zrodla
s = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
s = re.sub(r'//[^\n]*', '', s)
s = re.sub(r'"(\\.|[^"\\])*"', '""', s)

def split_args(text):
    """dzieli po przecinkach na nawiasach 0"""
    depth = 0; out = []; cur = ""
    for ch in text:
        if ch in "([{": depth += 1
        elif ch in ")]}": depth -= 1
        if ch == "," and depth == 0:
            out.append(cur); cur = ""
        else:
            cur += ch
    if cur.strip(): out.append(cur)
    return out

# 3) znajdz wywolania makr
problems = []; checked = 0
for m in re.finditer(r'\b(IDirect3D\w+|IDirect3DDevice9\w+|IDirect3DSurface9\w+)\s*\(', s):
    name = m.group(1)
    if name not in macro_args:
        continue
    # znajdz pasujacy nawias zamykajacy
    i = m.end(); depth = 1; start = i
    while i < len(s) and depth > 0:
        if s[i] == "(": depth += 1
        elif s[i] == ")": depth -= 1
        i += 1
    argtext = s[start:i-1]
    args = split_args(argtext)
    n = len([a for a in args if a.strip() != ""])
    line = s[:m.start()].count("\n") + 1
    checked += 1
    if n != macro_args[name]:
        problems.append((line, name, n, macro_args[name], argtext.strip()[:120]))

print("Sprawdzono %d wywolan makr IDirect3D*." % checked)
if problems:
    print("\n*** NIEZGODNA LICZBA ARGUMENTOW ***")
    for (line, name, got, want, txt) in problems:
        print("  linia ~%d: %s  -> masz %d, makro chce %d" % (line, name, got, want))
        print("      %s" % txt)
    sys.exit(1)
print("Wszystkie zgadzaja sie z d3d9.h.")

# 4) dodatkowe: identyfikatory D3D* obecne w .c a nieobecne w naglowkach
hdrs = hdr
for extra in ("src/hdr_d3d9types.h", "src/hdr_d3d9caps.h"):
    try: hdrs += open(extra, encoding="utf-8", errors="replace").read()
    except OSError: pass
ids = set(re.findall(r'\b(D3D[A-Z0-9_]{2,}|D3DFMT_\w+|D3DPOOL_\w+|D3DTEXF_\w+|D3DCREATE_\w+|D3DPRESENT_\w+|D3DSWAPEFFECT_\w+|D3DMULTISAMPLE_\w+|D3DBACKBUFFER_TYPE_\w+|D3DRTYPE_\w+|D3DDEVTYPE_\w+|D3DLOCK_\w+)\b', s))
missing = sorted(i for i in ids if not re.search(r'\b'+re.escape(i)+r'\b', hdrs))
print("\nIdentyfikatory D3D* w .c: %d" % len(ids))
print("NIEOBECNE w naglowkach mingw-w64: %s" % (missing if missing else "BRAK"))
