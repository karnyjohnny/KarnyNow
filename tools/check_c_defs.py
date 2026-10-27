# -*- coding: utf-8 -*-
"""Weryfikator: czy kazda funkcja uzyta w T4_d3d9_present.c jest zdefiniowana
albo zadeklarowana w naglowkach. Bez kompilatora - czysto statycznie."""
import re, sys

SRC = "tests/T4_d3d9_present.c"
s = open(SRC, encoding="utf-8", errors="replace").read()

# usun komentarze i stringi
c = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
c = re.sub(r'//[^\n]*', '', c)
c = re.sub(r'"(\\.|[^"\\])*"', '""', c)

# definicje funkcji w pliku: "static <typ> nazwa(" lub "<typ> nazwa(" na poczatku linii
defined = set()
for m in re.finditer(r'^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', c, re.M):
    defined.add(m.group(1))

# znane z Windows SDK / CRT (biala lista - te na pewno istnieja)
SDK = set("""
main printf fprintf snprintf _snprintf sprintf vprintf fopen fclose fread fwrite fseek ftell
fflush fputs fputc fgets malloc calloc realloc free memcpy memset memmove strlen strncmp strcmp
strncpy strstr strchr strrchr strerror atoi atol atof strtoul strtol qsort bsearch abs
Direct3DCreate9 CreateWindowExW RegisterClassW DefWindowProcW PostQuitMessageW LoadCursorW
GetModuleHandleW DestroyWindow GetDC ReleaseDC CreateDIBSection DeleteObject SetDIBitsToDevice
QueryPerformanceCounter QueryPerformanceFrequency GetProcessTimes GetCurrentProcess
LoadLibraryA FreeLibrary GetProcAddress GetSystemMetrics EnumDisplaySettingsW
GetConsoleMode SetConsoleMode GetStdHandle WriteConsoleW
IDirect3D9_CheckDeviceFormat IDirect3D9_GetAdapterCount
""".split())

# wywolania funkcji w kodzie
called = {}
for m in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', c):
    name = m.group(1)
    # pomijamy slowa kluczowe i makra kontrolne
    if name in ("if","for","while","switch","return","sizeof","defined","do","else","case"):
        continue
    called.setdefault(name, []).append(c[:m.start()].count("\n")+1)

missing = {}
for name, lines in sorted(called.items()):
    if name in defined or name in SDK:
        continue
    if name.startswith(("IDirect3D","j","elem_","stage_","emit_","device_","conv_","fmt_","now_","cmp_","make_","mark","body_","append_","wrs_","json_")):
        # nasze wlasne - powinny byc w defined; jesli nie ma, to blad
        if name not in defined:
            missing[name] = lines
    else:
        missing[name] = lines

print("Funkcje zdefiniowane w pliku: %d" % len(defined))
print("Nazwy wywolywane a NIE zdefiniowane lokalnie i nie na bialej liscie SDK:")
if not missing:
    print("  BRAK - wszystko zdefiniowane lub z SDK")
else:
    for n,l in sorted(missing.items()):
        print("  %-42s pierwsze uzycie: linia %d  (razy: %d)" % (n, l[0], len(l)))
sys.exit(1 if missing else 0)
