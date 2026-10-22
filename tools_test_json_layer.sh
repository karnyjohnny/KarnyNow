#!/usr/bin/env bash
# Test SAMEJ warstwy zapisu JSON z T4, uruchamiany na Linuxie (bez D3D9).
# Powod: v3 oddal pliki PUSTE, a v4 pliki z zagniezdzonym obiektem bez klucza.
# Oba bledy byly w warstwie zapisu, nie w D3D9 - i oba dalo sie zlapac w 2 sekundy
# na hostie, bez angazowania maszyny uzytkownika.
#
# Uzycie: bash tools_test_json_layer.sh
set -eu
TMP=$(mktemp -d)
python3 - "$TMP" <<'PY'
import sys
out=sys.argv[1]
s=open("tests/T4_d3d9_present.c",encoding="utf-8").read()
start=s.find("/* ------------------------------------------------- zapis JSON (v4)")
end=s.find("/* --------------------------------------------------------------- helpery */")
assert start>0 and end>start, "nie znaleziono warstwy JSON"
layer=s[start:end]
assert "static void json_begin" in layer, "json_begin nie miesci sie w wycinku"
test = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef long HRESULT;
#define _snprintf snprintf
''' + layer + r'''
int main(void)
{
    int i;
    strcpy(g_outpath, OUTPATH);
    { FILE *f = fopen(g_outpath,"wb"); if (f) fclose(f); }
    json_begin("all", 2, 0, 200);
    elem_begin();
    jtext("name","info"); juint("count", 42); jbool("ok", 1);
    jopen_obj("nested"); jint("a",-5); jnum("b",3.14159); jclose_obj();
    jopen_arr("list");
    for (i=0;i<3;i++){ jel_open(); jint("i",i); jtext("s","x\"y\\z"); jel_close(); }
    jclose_arr();
    elem_end("info");
    elem_begin();
    jtext("name","formats"); jhr("hr", (HRESULT)0x80004002L);
    elem_end("formats");
    elem_begin();
    jtext("name","big");
    for (i=0;i<2000;i++){ char kb[24]; sprintf(kb,"k%d",i); jint(kb, i); }
    elem_end("big");
    return 0;
}
'''.replace("OUTPATH", '"%s/out.json"' % out)
open(out+"/t.c","w").write(test)
PY
gcc -std=c11 -O1 -w -o "$TMP/t" "$TMP/t.c"
"$TMP/t"
python3 - "$TMP/out.json" <<'PY'
import json,sys
raw=open(sys.argv[1],encoding="utf-8").read()
d=json.loads(raw)                      # rzuci wyjatkiem, jesli JSON jest zly
assert d["requested_stage"]=="all" and d["case"]==2 and d["iters"]==200, "naglowek"
assert len(d["stages"])==3, "oczekiwano 3 elementow, jest %d"%len(d["stages"])
assert d["stages"][0]["stage"]=="info" and d["stages"][0]["count"]==42
assert d["stages"][0]["nested"]=={"a":-5,"b":3.14159}, "nested"
assert d["stages"][0]["list"][0]["s"]=='x"y\\z', "escapowanie"
assert d["stages"][1]["hr"]=="0x80004002", "hr hex"
assert d["stages"][2].get("k1999")==1999, "duzy element"
assert len(d["stages"][2])>2000, "duzy element: liczba pol"
print("TEST WARSTWY JSON: PASS  (%d bajtow, 3 elementy, escapowanie/hex/duze dane OK)"%len(raw))
PY
rm -rf "$TMP"
