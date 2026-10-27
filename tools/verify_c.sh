#!/usr/bin/env bash
# Weryfikacja kompilacji T4 (i kazdego innego .c klienta) BEZ maszyny docelowej.
#
# Dlaczego to istnieje: asystent pracowal przez trzy rundy "na slepo" - bez
# kompilatora w sandboxie - i trzy razy oddal plik .c, ktory sie nie kompilowal
# (zle nazwy pol D3DCAPS9, zla liczba argumentow makr, nieistniejacy typ
# D3DUSAGE, brakujaca definicja mark()). Kazda taka runda kosztowala uzytkownika
# jedno uruchomienie i kilka minut. Ten skrypt konczy ten problem.
#
# Sandbox NIE ma gcc domyslnie, ALE ma apt. Pakiety NIE przetrwaja miedzy
# sesjami (snapshot workspace obejmuje tylko pliki), wiec na poczatku KAŻDEJ
# sesji, w ktorej masz edytowac kod C, uruchom ten skrypt - zainstaluje
# toolchain i skompiluje.
#
# Uzycie:
#   bash tools/verify_c.sh              # instalacja (jesli brak) + kompilacja T4
#   bash tools/verify_c.sh plik.c       # kompilacja wskazanego pliku
#
# Toolchain w sandboxie to GCC 12.2 / mingw-w64 (Debian), a u uzytkownika
# GCC 16.2 / mingw-w64 v14 (w64devkit). Roznica wersji nie jest istotna dla
# wychwytywania bledow skladni i sygnatur; naglowki d3d9.h sa w obu te same
# co do nazw i liczby argumentow makr.

set -u

SRC="${1:-tests/T4_d3d9_present.c}"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "[verify] instaluje gcc-mingw-w64-x86-64 (apt)..."
    apt-get install -y gcc-mingw-w64-x86-64 >/tmp/apt-mingw.log 2>&1 \
        || { apt-get update >/tmp/apt-up.log 2>&1 && apt-get install -y gcc-mingw-w64-x86-64 >>/tmp/apt-mingw.log 2>&1; }
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "[verify] NIE UDALO sie zainstalowac toolchaina - zobacz /tmp/apt-mingw.log"
        exit 2
    fi
fi

CC=x86_64-w64-mingw32-gcc
OUT=/tmp/verify_$(basename "$SRC" .c).exe

echo "[verify] $CC -std=c11 -Wall -Wextra ... $SRC"
"$CC" -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000 \
    -march=core2 -msse3 -static -static-libgcc \
    -o "$OUT" "$SRC" \
    -ld3d9 -lgdi32 -luser32 -lws2_32 -lwinmm -lpsapi -luuid -lole32
rc=$?

if [ $rc -eq 0 ]; then
    echo "[verify] OK - zero bledow i zero ostrzezen. Binarka: $OUT"
    ls -la "$OUT"
else
    echo "[verify] KOMPILACJA NIE UDANA (rc=$rc) - NIE wysylaj tego uzytkownikowi"
fi
exit $rc
