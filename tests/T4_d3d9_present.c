/*
 * T4 v4 - D3D9 presentation probe z IZOLACJA CRASHA (etapy + podkroki).
 * Intel GMA 4500MHD (8086:2A42), Windows 7 SP1, mingw GCC 16.2.0.
 *
 * Dlaczego v3 (historia awarii):
 *   v1 - caly JSON buforowany i zapisywany na koncu -> 0xC0000005 bez zadnego sladu.
 *   v2 - jflush() resetowal bufor, wiec pozniejsze etapy nadpisywaly wczesniejsze.
 *   v3 - fseek/ftell po wlasnym wyjsciu + "seal" -> pliki wyszly PUSTE (0 bajtow).
 *   v4 - akumulator w RAM + PRZEPISANIE CALEGO PLIKU przy kazdym elemencie.
 *        Zero fseek/ftell. Dodatkowo updatesurf rozbity na 6 podkrokow, bo to on
 *        crashowal z 0xC0000005 na x64 I x86 - teraz dowiemy sie, ktore wywolanie. Do tego kazda
 *        ryzykowna operacja leci w OSOBNYM PROCESIE (harness przekazuje --stage
 *        i --case), wiec kod powrotu 0xC0000005 wskazuje DOKLADNIE winnego.
 *
 * Zasada, ktora z tej awarii wynika dla kodu produkcyjnego:
 *   NA STARYM STEROWNIKU INTEL NIE WOLNO WYWOLYWAC StretchRect/UpdateSurface
 *   NA NV12 BEZ WCZESNIEJSZEGO CheckDeviceFormatConversion. Ten probe dlatego
 *   najpierw pyta, a dopiero potem probuje - i to samo musi robic nasz klient.
 *
 * Uzycie:
 *   t4.exe --out=PLIK.json --stage=info|formats|device|surfaces|updatesurf|stretch|bench|all
 *          [--case=0..4] [--iters=200]
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define WINVER       0x0601

#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef D3DFMT_NV12
#define D3DFMT_NV12 ((D3DFORMAT)MAKEFOURCC('N','V','1','2'))
#endif
#ifndef D3DFMT_YUY2
#define D3DFMT_YUY2 ((D3DFORMAT)MAKEFOURCC('Y','U','Y','2'))
#endif
#ifndef D3DFMT_UYVY
#define D3DFMT_UYVY ((D3DFORMAT)MAKEFOURCC('U','Y','V','Y'))
#endif
#define FMT_YUY2 D3DFMT_YUY2
#define FMT_UYVY D3DFMT_UYVY
#define IS_NV12(f) ((DWORD)(f) == (DWORD)D3DFMT_NV12)

/* ------------------------------------------------- zapis JSON (v4)
 *
 * Historia awarii zapisu:
 *   v1 - caly JSON buforowany, zapis na koncu      -> crash zabral WSZYSTKO
 *   v3 - fseek/ftell po wlasnym wyjsciu + seal     -> pliki wyszly PUSTE (0 bajtow)
 *   v4 - akumulator w RAM + PRZEPISANIE CALEGO PLIKU przy kazdym flush.
 *        Zero fseek/ftell. Plik jest zawsze poprawnym, zamknietym JSON-em.
 *
 * To jest bezpieczne, bo kazdy proces wykonuje JEDEN etap, a jego wyjscie to
 * najwyzej kilkadziesiat KB. Koszt przepisania ~1 ms.
 *
 * Zasada, ktora z tego wynika dla klienta produkcyjnego: diagnostyke zapisuj
 * przez przepisanie calej struktury albo przez append-only z jawnym flush,
 * nigdy przez mix fseek po wlasnym wyjsciu.
 */

#define ACC_CAP  (1u << 20)      /* 1 MiB na wszystkie elementy etapu */
#define BODY_CAP (1u << 18)      /* 256 KiB na jeden element          */

static char  *g_acc;             /* zakumulowane elementy: "{...},{...}"  */
static size_t g_acc_len;
static char  *g_body;            /* biezacy element                       */
static size_t g_len;
static char   g_outpath[1024];
static char   g_hdr[512];

static void acc_init(void)
{
    if (!g_acc)  g_acc  = (char *)malloc(ACC_CAP);
    if (!g_body) g_body = (char *)malloc(BODY_CAP);
    g_acc_len = 0; g_len = 0;
    if (g_acc)  g_acc[0]  = 0;
    if (g_body) g_body[0] = 0;
}

static void jraw(const char *s)
{
    size_t n;
    if (!g_body) return;
    n = strlen(s);
    if (g_len + n + 2 >= BODY_CAP) return;
    memcpy(g_body + g_len, s, n);
    g_len += n;
    g_body[g_len] = 0;
}

static void jesc_w(const char *s)
{
    char e[700];
    size_t o = 0;
    if (!s) s = "";
    for (; *s && o + 8 < sizeof(e); ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { e[o++] = '\\'; e[o++] = (char)c; }
        else if (c == '\n') { e[o++] = '\\'; e[o++] = 'n'; }
        else if (c == '\r') { e[o++] = '\\'; e[o++] = 'r'; }
        else if (c == '\t') { e[o++] = '\\'; e[o++] = 't'; }
        else if (c < 0x20) { o += (size_t)_snprintf(e + o, sizeof(e) - o, "\\u%04x", (unsigned)c); }
        else e[o++] = (char)c;
    }
    e[o] = 0;
    jraw(e);
}

static void jnum(const char *k, double v) { char b[160]; _snprintf(b, sizeof(b), "\"%s\":%.6g,", k, v); jraw(b); }
static void jint(const char *k, long v)   { char b[160]; _snprintf(b, sizeof(b), "\"%s\":%ld,", k, v); jraw(b); }
static void juint(const char *k, unsigned long v) { char b[160]; _snprintf(b, sizeof(b), "\"%s\":%lu,", k, v); jraw(b); }
static void jbool(const char *k, int v)   { char b[160]; _snprintf(b, sizeof(b), "\"%s\":%s,", k, v ? "true" : "false"); jraw(b); }
static void jhr(const char *k, HRESULT hr){ char b[160]; _snprintf(b, sizeof(b), "\"%s\":\"0x%08lX\",", k, (unsigned long)(hr & 0xFFFFFFFFu)); jraw(b); }
static void jtext(const char *k, const char *v)
{
    char b[160];
    _snprintf(b, sizeof(b), "\"%s\":\"", k);
    jraw(b); jesc_w(v); jraw("\",");
}
static void jopen_obj(const char *k) { char b[160]; _snprintf(b, sizeof(b), "\"%s\":{", k); jraw(b); }
static void jopen_arr(const char *k) { char b[160]; _snprintf(b, sizeof(b), "\"%s\":[", k); jraw(b); }
static void jtrim(void)  { if (g_len && g_body[g_len - 1] == ',') { g_len--; g_body[g_len] = 0; } }
static void jclose_obj(void) { jtrim(); jraw("},"); }
static void jclose_arr(void) { jtrim(); jraw("],"); }
static void jel_open(void)   { jraw("{"); }
static void jel_close(void)  { jtrim(); jraw("},"); }

/* Przepisuje CALY plik: naglowek + zakumulowane elementy + domkniecie.
 * Wolane po kazdym elemencie, wiec plik na dysku jest zawsze poprawny. */
static void seal_file(void)
{
    FILE *f;
    if (!g_outpath[0]) return;
    f = fopen(g_outpath, "wb");
    if (!f) return;
    /* g_hdr KONCZY SIE PRZECINKIEM (ostatnie pole to "iters":N,) - dlatego
     * tu doklejamy sama nazwe klucza i otwarcie tablicy, BEZ dodatkowego "{".
     * Blad v4: bylo fputs("{\"stages\":[") -> powstawal zagniezdzony obiekt
     * bez klucza i plik nie byl poprawnym JSON-em. Zlapane przez test warstwy
     * JSON na Linuxie (patrz tools/test_json_layer.sh). */
    fputs(g_hdr, f);
    fputs("\"stages\":[", f);
    if (g_acc && g_acc_len) fwrite(g_acc, 1, g_acc_len, f);
    fputs("]}", f);
    fflush(f);
    fclose(f);
}

static void elem_begin(void)
{
    if (!g_body) acc_init();
    g_len = 0;
    if (g_body) g_body[0] = 0;
    jraw("{");
}

static void elem_end(const char *stage)
{
    char b[160];
    size_t need;
    jtrim();
    _snprintf(b, sizeof(b), ",\"stage\":\"%s\"}", stage);
    jraw(b);
    if (!g_acc) return;
    need = g_acc_len + g_len + 2;
    if (need >= ACC_CAP) { seal_file(); return; }   /* nie przepełniamy */
    if (g_acc_len) { g_acc[g_acc_len++] = ','; g_acc[g_acc_len] = 0; }
    memcpy(g_acc + g_acc_len, g_body, g_len);
    g_acc_len += g_len;
    g_acc[g_acc_len] = 0;
    seal_file();
    g_len = 0;
    if (g_body) g_body[0] = 0;
}

/* Przygotowuje naglowek dokumentu. Zapis wykonuje seal_file(). */
static void json_begin(const char *stage_requested, int caseidx, int which, int iters)
{
    acc_init();
    _snprintf(g_hdr, sizeof(g_hdr),
              "{\"test\":\"T4_d3d9_present_v4\",\"built_at\":\"%s %s\","
              "\"requested_stage\":\"%s\",\"case\":%d,\"which\":%d,\"iters\":%d,",
              __DATE__, __TIME__, stage_requested, caseidx, which, iters);
    seal_file();   /* zapisz od razu: crash przed pierwszym elementem i tak da poprawny plik */
}

/* Znacznik postepu na stderr - jedyna rzecz, ktora przezyla nawet najgorszy crash. */
static void mark(const char *s)
{
    fprintf(stderr, "[t4] %s\n", s);
    fflush(stderr);
}

/* --------------------------------------------------------------- helpery */

static const char *fmt_name(D3DFORMAT f)
{
    static char bufs[16][24];
    static int  idx;
    DWORD d = (DWORD)f;
    char *b;
    switch (f) {
    case D3DFMT_UNKNOWN:    return "UNKNOWN";
    case D3DFMT_R8G8B8:     return "R8G8B8";
    case D3DFMT_A8R8G8B8:   return "A8R8G8B8";
    case D3DFMT_X8R8G8B8:   return "X8R8G8B8";
    case D3DFMT_R5G6B5:     return "R5G6B5";
    case D3DFMT_X1R5G5B5:   return "X1R5G5B5";
    case D3DFMT_A1R5G5B5:   return "A1R5G5B5";
    case D3DFMT_A8:         return "A8";
    case D3DFMT_D32:        return "D32";
    case D3DFMT_D24S8:      return "D24S8";
    case D3DFMT_D24X8:      return "D24X8";
    case D3DFMT_D16:        return "D16";
    case D3DFMT_VERTEXDATA: return "VERTEXDATA";
    default: break;
    }
    if (IS_NV12(f))    return "NV12";
    if (f == FMT_YUY2) return "YUY2";
    if (f == FMT_UYVY) return "UYVY";
    b = bufs[idx++ & 15];
    if ((d & 0xFF) >= 0x20 && (d & 0xFF) <= 0x7E &&
        ((d >> 8) & 0xFF) >= 0x20 && ((d >> 8) & 0xFF) <= 0x7E &&
        ((d >> 16) & 0xFF) >= 0x20 && ((d >> 16) & 0xFF) <= 0x7E &&
        ((d >> 24) & 0xFF) >= 0x20 && ((d >> 24) & 0xFF) <= 0x7E) {
        _snprintf(b, 24, "%c%c%c%c", (int)(d & 0xFF), (int)((d >> 8) & 0xFF),
                  (int)((d >> 16) & 0xFF), (int)((d >> 24) & 0xFF));
    } else {
        _snprintf(b, 24, "0x%08lX", (unsigned long)d);
    }
    return b;
}

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    if (freq.QuadPart == 0) return 0.0;
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void emit_timing(const char *key, double *s, int n, int iters)
{
    double sum = 0.0;
    int i, p95;
    if (!s || n <= 0) return;
    qsort(s, (size_t)n, sizeof(double), cmp_d);
    for (i = 0; i < n; ++i) sum += s[i];
    p95 = (int)((double)n * 0.95);
    if (p95 >= n) p95 = n - 1;
    jopen_obj(key);
    jint("iters", iters);
    jnum("ms_mean", sum / (double)n);
    jnum("ms_median", s[n / 2]);
    jnum("ms_p95", s[p95]);
    jnum("ms_min", s[0]);
    jnum("ms_max", s[n - 1]);
    jnum("fps_at_mean", sum > 0.0 ? 1000.0 / (sum / (double)n) : 0.0);
    jclose_obj();
}

/* ------------------------------------------------------------------ okno */

static const wchar_t CLS_NAME[] = L"KarnyNowT4";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY)    { PostQuitMessage(0); return 0; }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

static HWND make_window(int w, int h)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = CLS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) return NULL;
    return CreateWindowExW(WS_EX_TOPMOST, CLS_NAME, L"KarnyNow T4 probe",
                           WS_POPUP | WS_VISIBLE, 0, 0, w, h,
                           NULL, NULL, wc.hInstance, NULL);
}

/* ---------------------------------------------------------------- globals */

static IDirect3D9       *g_d3d;
static IDirect3DDevice9 *g_dev;
static D3DDISPLAYMODE    g_dm;
static HWND              g_hwnd;
static HRESULT           g_hr_hwvp = E_FAIL, g_hr_swvp = E_FAIL;
static DWORD             g_behavior;
static int               g_screen_w = 1280, g_screen_h = 800;
static int               g_src_w = 960, g_src_h = 540;
static int               g_iters = 200;

static int device_alive(void)
{
    if (!g_dev) return 0;
    return SUCCEEDED(IDirect3DDevice9_TestCooperativeLevel(g_dev)) ? 1 : 0;
}

/* Pyta sterownika, czy konwersja src->dst jest dozwolona. To jest wlasnie ta
 * kontrola, ktorej brak prawdopodobnie zabil v1. */
static int conv_allowed(D3DFORMAT src, D3DFORMAT dst)
{
    HRESULT hr = IDirect3D9_CheckDeviceFormatConversion(g_d3d, D3DADAPTER_DEFAULT,
                                                        D3DDEVTYPE_HAL, src, dst);
    return SUCCEEDED(hr) ? 1 : 0;
}

/* ------------------------------------------------------------- etap: info */

static void stage_info(void)
{
    D3DCAPS9 caps;
    HRESULT hr;

    mark("stage_info");
    elem_begin();
    jtext("built_at", __DATE__ " " __TIME__);
    jint("pointer_bits", (long)(sizeof(void *) * 8));
#ifdef __GNUC__
    jint("gcc_major", __GNUC__);
    jint("gcc_minor", __GNUC_MINOR__);
#endif
    jint("adapter_count", (long)IDirect3D9_GetAdapterCount(g_d3d));

    jopen_obj("display_mode");
    jint("width", (long)g_dm.Width);
    jint("height", (long)g_dm.Height);
    jint("refresh_hz", (long)g_dm.RefreshRate);
    jtext("format", fmt_name(g_dm.Format));
    jclose_obj();

    {
        D3DADAPTER_IDENTIFIER9 id;
        char b[80];
        memset(&id, 0, sizeof(id));
        hr = IDirect3D9_GetAdapterIdentifier(g_d3d, D3DADAPTER_DEFAULT, 0, &id);
        jopen_obj("adapter_identifier");
        jhr("hr", hr);
        jtext("driver", id.Driver);
        jtext("description", id.Description);
        jtext("device_name", id.DeviceName);
        _snprintf(b, sizeof(b), "%04lX:%04lX",
                  (unsigned long)id.VendorId, (unsigned long)id.DeviceId);
        jtext("pci_id", b);
        _snprintf(b, sizeof(b), "%016llX", id.DriverVersion.QuadPart);
        jtext("driver_version_hex", b);
        jclose_obj();
    }

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3D9_GetDeviceCaps(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    jopen_obj("caps_hal");
    jhr("hr", hr);
    if (SUCCEEDED(hr)) {
        juint("caps", (unsigned long)caps.Caps);
        juint("caps2", (unsigned long)caps.Caps2);
        juint("dev_caps", (unsigned long)caps.DevCaps);
        jbool("hw_transform_and_light", (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0);
        jbool("pure_device", (caps.DevCaps & D3DDEVCAPS_PUREDEVICE) != 0);
        jbool("hw_rasterization", (caps.DevCaps & D3DDEVCAPS_HWRASTERIZATION) != 0);
        jint("max_texture_width", (long)caps.MaxTextureWidth);
        jint("max_texture_height", (long)caps.MaxTextureHeight);
        juint("presentation_intervals", (unsigned long)caps.PresentationIntervals);
        jbool("present_interval_immediate", (caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) != 0);
        jbool("present_interval_one", (caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE) != 0);
        juint("dev_caps2", (unsigned long)caps.DevCaps2);
        jbool("can_stretchrect_from_textures", (caps.DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES) != 0);
        juint("stretch_rect_filter_caps", (unsigned long)caps.StretchRectFilterCaps);
        jbool("srf_point",  (caps.StretchRectFilterCaps & D3DTEXF_POINT) != 0);
        jbool("srf_linear", (caps.StretchRectFilterCaps & D3DTEXF_LINEAR) != 0);
        juint("vertex_shader_version", (unsigned long)caps.VertexShaderVersion);
        juint("pixel_shader_version", (unsigned long)caps.PixelShaderVersion);
        jint("num_simultaneous_rts", (long)caps.NumSimultaneousRTs);
        juint("vertex_processing_caps", (unsigned long)caps.VertexProcessingCaps);
    }
    jclose_obj();

    {
        DEVMODEW dvm;
        memset(&dvm, 0, sizeof(dvm));
        dvm.dmSize = (WORD)sizeof(dvm);
        jopen_obj("current_display_mode");
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dvm)) {
            juint("width", (unsigned long)dvm.dmPelsWidth);
            juint("height", (unsigned long)dvm.dmPelsHeight);
            juint("refresh_hz", (unsigned long)dvm.dmDisplayFrequency);
            juint("bits_per_pel", (unsigned long)dvm.dmBitsPerPel);
        } else {
            jtext("error", "EnumDisplaySettingsW failed");
        }
        jclose_obj();

        {
            int mode = 0, listed = 0, best60 = 0, maxhz = 0;
            jopen_arr("modes_1280x800");
            for (;;) {
                DEVMODEW m2;
                memset(&m2, 0, sizeof(m2));
                m2.dmSize = (WORD)sizeof(m2);
                if (!EnumDisplaySettingsW(NULL, (DWORD)mode, &m2)) break;
                if (m2.dmPelsWidth == 1280 && m2.dmPelsHeight == 800 && m2.dmBitsPerPel >= 16) {
                    if (listed < 60) {
                        jel_open();
                        juint("hz", (unsigned long)m2.dmDisplayFrequency);
                        juint("bpp", (unsigned long)m2.dmBitsPerPel);
                        jel_close();
                    }
                    if (m2.dmDisplayFrequency >= 59 && m2.dmDisplayFrequency <= 61) best60 = 1;
                    if ((int)m2.dmDisplayFrequency > maxhz) maxhz = (int)m2.dmDisplayFrequency;
                    listed++;
                }
                mode++;
                if (mode > 8192) break;
            }
            jclose_arr();
            jbool("mode_1280x800_at_60hz_available", best60);
            jint("modes_1280x800_count", listed);
            jint("modes_1280x800_max_hz_seen", maxhz);
        }
    }
    elem_end("info");
}

/* ---------------------------------------------------------- etap: formats */

static void stage_formats(void)
{
    static const D3DFORMAT targets[] = {
        D3DFMT_NV12, FMT_YUY2, FMT_UYVY,
        D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5
    };
    static const DWORD usages[] = {
        0u, (DWORD)D3DUSAGE_RESTRICTED_CONTENT, (DWORD)D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING
    };
    size_t t, u;
    int d, p;

    mark("stage_formats");
    elem_begin();

    jopen_arr("check_device_format");
    for (d = 0; d < 2; ++d) {
        D3DDEVTYPE dt = d ? D3DDEVTYPE_REF : D3DDEVTYPE_HAL;
        for (t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
            for (u = 0; u < sizeof(usages) / sizeof(usages[0]); ++u) {
                HRESULT h2 = IDirect3D9_CheckDeviceFormat(g_d3d, D3DADAPTER_DEFAULT, dt,
                                                          g_dm.Format, usages[u],
                                                          D3DRTYPE_SURFACE, targets[t]);
                jel_open();
                jtext("device_type", dt == D3DDEVTYPE_HAL ? "HAL" : "REF");
                jtext("target_format", fmt_name(targets[t]));
                juint("usage", (unsigned long)usages[u]);
                jhr("hr", h2);
                jbool("ok", SUCCEEDED(h2));
                jel_close();
            }
        }
    }
    jclose_arr();

    jopen_arr("check_device_format_conversion");
    for (d = 0; d < 2; ++d) {
        D3DDEVTYPE dt = d ? D3DDEVTYPE_REF : D3DDEVTYPE_HAL;
        D3DFORMAT pairs[5][2];
        pairs[0][0] = D3DFMT_NV12;     pairs[0][1] = D3DFMT_X8R8G8B8;
        pairs[1][0] = D3DFMT_NV12;     pairs[1][1] = D3DFMT_A8R8G8B8;
        pairs[2][0] = FMT_YUY2;        pairs[2][1] = D3DFMT_X8R8G8B8;
        pairs[3][0] = D3DFMT_X8R8G8B8; pairs[3][1] = D3DFMT_NV12;
        pairs[4][0] = D3DFMT_NV12;     pairs[4][1] = D3DFMT_R5G6B5;
        /* API ma 4 parametry (Adapter, DeviceType, SourceFormat, TargetFormat) -
         * sprawdzone w d3d9.h mingw-w64; makro jest 5-arg z 'p'. */
        for (p = 0; p < 5; ++p) {
            HRESULT h2 = IDirect3D9_CheckDeviceFormatConversion(g_d3d, D3DADAPTER_DEFAULT,
                                                                dt, pairs[p][0], pairs[p][1]);
            jel_open();
            jtext("device_type", dt == D3DDEVTYPE_HAL ? "HAL" : "REF");
            jtext("src", fmt_name(pairs[p][0]));
            jtext("dst", fmt_name(pairs[p][1]));
            jhr("hr", h2);
            jbool("ok", SUCCEEDED(h2));
            jel_close();
        }
    }
    jclose_arr();

    jopen_obj("device_type_check");
    {
        HRESULT h1 = IDirect3D9_CheckDeviceType(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                g_dm.Format, g_dm.Format, TRUE);
        HRESULT h2 = IDirect3D9_CheckDeviceType(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                g_dm.Format, D3DFMT_X8R8G8B8, TRUE);
        jhr("hal_windowed_same_fmt", h1);
        jbool("hal_windowed_same_fmt_ok", SUCCEEDED(h1));
        jhr("hal_windowed_x8r8g8b8", h2);
        jbool("hal_windowed_x8r8g8b8_ok", SUCCEEDED(h2));
    }
    jclose_obj();
    elem_end("formats");
}

/* ----------------------------------------------------------- etap: device */

static void stage_device(void)
{
    mark("stage_device");
    elem_begin();
    jopen_obj("create_device");
    jbool("window_created", g_hwnd != NULL);
    jhr("hal_hardware_vp", g_hr_hwvp);
    jhr("hal_software_vp", g_hr_swvp);
    jbool("ok", g_dev != NULL);
    jtext("behavior", (g_behavior & D3DCREATE_HARDWARE_VERTEXPROCESSING)
          ? "HARDWARE_VERTEXPROCESSING" : "SOFTWARE_VERTEXPROCESSING");
    if (g_dev) jhr("test_cooperative_level", IDirect3DDevice9_TestCooperativeLevel(g_dev));
    jclose_obj();

    if (g_dev) {
        IDirect3DSurface9 *bb = NULL;
        HRESULT h6 = IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        jopen_obj("backbuffer");
        jhr("hr", h6);
        jbool("ok", SUCCEEDED(h6) && bb != NULL);
        if (bb) {
            D3DSURFACE_DESC sd;
            memset(&sd, 0, sizeof(sd));
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(bb, &sd))) {
                jint("width", (long)sd.Width);
                jint("height", (long)sd.Height);
                jtext("format", fmt_name(sd.Format));
                jint("usage", (long)sd.Usage);
                jint("pool", (long)sd.Pool);
                jint("multi_sample_type", (long)sd.MultiSampleType);
            }
            IDirect3DSurface9_Release(bb);
        }
        jclose_obj();
    }
    elem_end("device");
}

/* --------------------------------------------------------- etap: surfaces */

static void stage_surfaces(void)
{
    IDirect3DSurface9 *nv12_a = NULL, *nv12_b = NULL, *yuy2 = NULL, *rgb = NULL, *sysmem = NULL;
    IDirect3DSurface9 *rt_nv12 = NULL, *rt_rgb = NULL;
    HRESULT h1, h2, h3, h4, h5, r1, r2;
    IDirect3DSurface9 *list[5];
    const char *names[5];
    int i;

    mark("stage_surfaces");
    elem_begin();
    if (!device_alive()) {
        jtext("skipped", "device not alive");
        elem_end("surfaces");
        return;
    }

    h1 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                      D3DFMT_NV12, D3DPOOL_DEFAULT, &nv12_a, NULL);
    /* UWAGA: mingw-w64 nie daje tu parametru Usage, wiec D3DUSAGE_RESTRICTED_CONTENT
     * jest niewyrazalny przez CreateOffscreenPlainSurface. Zamiast tego tworzymy NV12
     * w rozmiarze EKRANU - to przypadek, ktory realnie nas interesuje (1:1 present). */
    h2 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_screen_w, (UINT)g_screen_h,
                                                      D3DFMT_NV12, D3DPOOL_DEFAULT, &nv12_b, NULL);
    h3 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                      FMT_YUY2, D3DPOOL_DEFAULT, &yuy2, NULL);
    h4 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                      D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &rgb, NULL);
    h5 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                      D3DFMT_NV12, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    r1 = IDirect3DDevice9_CreateRenderTarget(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                             D3DFMT_NV12, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_nv12, NULL);
    r2 = IDirect3DDevice9_CreateRenderTarget(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                             D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_rgb, NULL);

    jopen_obj("surface_creation");
    jhr("nv12_default_usage0", h1);
    jbool("nv12_default_usage0_ok", SUCCEEDED(h1) && nv12_a != NULL);
    jhr("nv12_screensize_default", h2);
    jbool("nv12_screensize_default_ok", SUCCEEDED(h2) && nv12_b != NULL);
    jhr("yuy2_default", h3);
    jbool("yuy2_default_ok", SUCCEEDED(h3) && yuy2 != NULL);
    jhr("x8r8g8b8_default", h4);
    jbool("x8r8g8b8_default_ok", SUCCEEDED(h4) && rgb != NULL);
    jhr("nv12_systemmem", h5);
    jbool("nv12_systemmem_ok", SUCCEEDED(h5) && sysmem != NULL);
    jhr("render_target_nv12", r1);
    jbool("render_target_nv12_ok", SUCCEEDED(r1) && rt_nv12 != NULL);
    jhr("render_target_x8r8g8b8", r2);
    jbool("render_target_x8r8g8b8_ok", SUCCEEDED(r2) && rt_rgb != NULL);
    jclose_obj();

    list[0] = nv12_a; names[0] = "nv12_default";
    list[1] = yuy2;   names[1] = "yuy2_default";
    list[2] = rgb;    names[2] = "x8r8g8b8_default";
    list[3] = sysmem; names[3] = "nv12_systemmem";
    list[4] = nv12_b; names[4] = "nv12_screensize";

    jopen_arr("surface_descs");
    for (i = 0; i < 5; ++i) {
        jel_open();
        jtext("name", names[i]);
        if (list[i]) {
            D3DSURFACE_DESC sd;
            D3DLOCKED_RECT lr;
            HRESULT lk;
            memset(&sd, 0, sizeof(sd));
            if (SUCCEEDED(IDirect3DSurface9_GetDesc(list[i], &sd))) {
                jint("width", (long)sd.Width);
                jint("height", (long)sd.Height);
                jtext("format", fmt_name(sd.Format));
                jint("usage", (long)sd.Usage);
            }
            memset(&lr, 0, sizeof(lr));
            lk = IDirect3DSurface9_LockRect(list[i], &lr, NULL, D3DLOCK_NOSYSLOCK);
            jhr("lock_nosyslock_hr", lk);
            if (SUCCEEDED(lk)) {
                jint("lock_pitch", (long)lr.Pitch);
                IDirect3DSurface9_UnlockRect(list[i]);
            } else {
                memset(&lr, 0, sizeof(lr));
                lk = IDirect3DSurface9_LockRect(list[i], &lr, NULL, 0);
                jhr("lock_plain_hr", lk);
                if (SUCCEEDED(lk)) {
                    jint("lock_pitch_plain", (long)lr.Pitch);
                    IDirect3DSurface9_UnlockRect(list[i]);
                }
            }
        } else {
            jtext("skipped", "not created");
        }
        jel_close();
    }
    jclose_arr();

    if (nv12_a) IDirect3DSurface9_Release(nv12_a);
    if (nv12_b) IDirect3DSurface9_Release(nv12_b);
    if (yuy2)   IDirect3DSurface9_Release(yuy2);
    if (rgb)    IDirect3DSurface9_Release(rgb);
    if (sysmem) IDirect3DSurface9_Release(sysmem);
    if (rt_nv12) IDirect3DSurface9_Release(rt_nv12);
    if (rt_rgb)  IDirect3DSurface9_Release(rt_rgb);
    elem_end("surfaces");
}

/* ------------------------------------------------------ etap: updatesurf */

static void stage_updatesurf(int sub)
{
    /* sub: -1 = wszystko po kolei (ryzykowne)
     *       0 = tylko CreateOffscreenPlainSurface SYSTEMMEM NV12
     *       1 = + CreateOffscreenPlainSurface DEFAULT NV12
     *       2 = + LockRect(SYSTEMMEM)
     *       3 = + wypelnienie bufora (czysto nasza pamiec - nie moze crashowac drivera)
     *       4 = + UnlockRect
     *       5 = + UpdateSurface(SYSTEMMEM -> DEFAULT)
     *
     * Dlaczego to jest rozbite: v3 crashowal tu z 0xC0000005 na x64 I x86.
     * Bez podkrokow wiedzielismy tylko "updatesurf". Harness uruchamia kazdy
     * podkrok w osobnym procesie, wiec dostajemy DOKLADNA nazwe wywolania,
     * ktore zabija driver - a to jest wiedza, ktora wchodzi do klienta jako
     * zakaz (patrz analiza/06 i HANDOFF D7).
     */
    IDirect3DSurface9 *sysmem = NULL, *dst = NULL;
    D3DLOCKED_RECT lr;
    HRESULT h5, h1, lk = E_FAIL;
    int locked = 0;

    mark("stage_updatesurf");
    elem_begin();
    jint("sub_step", sub);
    if (!device_alive()) { jtext("skipped", "device not alive"); elem_end("updatesurf"); return; }

    jopen_obj("update_surface_nv12");

    h5 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                      D3DFMT_NV12, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    jhr("create_systemmem", h5);
    jbool("systemmem_ok", SUCCEEDED(h5) && sysmem != NULL);
    if (sub == 0) { jclose_obj(); elem_end("updatesurf_sub0"); return; }

    h1 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                      D3DFMT_NV12, D3DPOOL_DEFAULT, &dst, NULL);
    jhr("create_default", h1);
    jbool("default_ok", SUCCEEDED(h1) && dst != NULL);
    if (sub == 1) { jclose_obj(); elem_end("updatesurf_sub1"); return; }

    if (!sysmem || !dst) { jtext("skipped", "surfaces not created"); jclose_obj(); elem_end("updatesurf"); return; }

    memset(&lr, 0, sizeof(lr));
    lk = IDirect3DSurface9_LockRect(sysmem, &lr, NULL, 0);
    jhr("lock_systemmem", lk);
    jbool("lock_ok", SUCCEEDED(lk));
    if (SUCCEEDED(lk)) {
        locked = 1;
        jint("systemmem_pitch", (long)lr.Pitch);
        jbool("bits_non_null", lr.pBits != NULL);
    }
    if (sub == 2) {
        if (locked) IDirect3DSurface9_UnlockRect(sysmem);
        jclose_obj(); elem_end("updatesurf_sub2"); return;
    }

    if (SUCCEEDED(lk) && lr.pBits && lr.Pitch > 0) {
        int x, y;
        unsigned char *yp = (unsigned char *)lr.pBits;
        int ypitch = (int)lr.Pitch;
        for (y = 0; y < g_src_h; ++y) {
            unsigned char *row = yp + (size_t)y * (size_t)ypitch;
            for (x = 0; x < g_src_w; ++x)
                row[x] = (unsigned char)((((x >> 4) + (y >> 4)) & 1) ? 235 : 16);
        }
        {
            unsigned char *cp = yp + (size_t)ypitch * (size_t)g_src_h;
            memset(cp, 128, (size_t)g_src_w * (size_t)(g_src_h / 2));
        }
        jbool("filled", 1);
    } else {
        jbool("filled", 0);
    }
    if (sub == 3) {
        if (locked) IDirect3DSurface9_UnlockRect(sysmem);
        jclose_obj(); elem_end("updatesurf_sub3"); return;
    }

    if (locked) { IDirect3DSurface9_UnlockRect(sysmem); locked = 0; }
    jbool("unlocked", 1);
    if (sub == 4) { jclose_obj(); elem_end("updatesurf_sub4"); return; }

    {
        HRESULT uh = IDirect3DDevice9_UpdateSurface(g_dev, sysmem, NULL, dst, NULL);
        jhr("update_surface", uh);
        jbool("ok", SUCCEEDED(uh));
    }
    jclose_obj();
    elem_end("updatesurf_sub5");

    if (sysmem) IDirect3DSurface9_Release(sysmem);
    if (dst) IDirect3DSurface9_Release(dst);
}

/* ---------------------------------------------------------- etap: stretch */

static void stage_stretch(int caseidx)
{
    static const char *case_names[5] = {
        "x8r8g8b8_to_backbuffer_scaled",   /* 0 - bazowy, na pewno bezpieczny */
        "yuy2_to_backbuffer_scaled",       /* 1 */
        "nv12_to_backbuffer_scaled",       /* 2 - glowny kandydat (960x540 -> 1280x800) */
        "nv12_screensize_to_backbuffer_1to1", /* 3 - przypadek realny: 1280x720 -> 1280x800 */
        "nv12_to_x8r8g8b8_offscreen"       /* 4 */
    };
    IDirect3DSurface9 *src = NULL, *dstsurf = NULL, *rgb = NULL, *bb = NULL;
    D3DFORMAT srcfmt = D3DFMT_X8R8G8B8, dstfmt = D3DFMT_X8R8G8B8;
    RECT srcrect, dstrect;
    HRESULT hc, hs;
    int g_case_w = 0, g_case_h = 0;

    mark("stage_stretch");
    elem_begin();
    jtext("case", (caseidx >= 0 && caseidx < 5) ? case_names[caseidx] : "unknown");
    jint("case_index", caseidx);

    if (!device_alive()) { jtext("skipped", "device not alive"); elem_end("stretch"); return; }
    if (caseidx < 0 || caseidx > 4) { jtext("skipped", "bad case index"); elem_end("stretch"); return; }

    srcrect.left = 0; srcrect.top = 0; srcrect.right = g_src_w; srcrect.bottom = g_src_h;
    dstrect.left = 0; dstrect.top = 0;  dstrect.right = g_screen_w; dstrect.bottom = g_screen_h;

    /* Case 3 to przypadek REALNY dla klienta: strumien 1280x720 ladowany do
     * powierzchni o wymiarach bliskich ekranowi i prezentowany praktycznie 1:1.
     * Dlatego powierzchnia i srcRect musza byc ekranowe, a nie 960x540. */
    {
        int cw = (caseidx == 3) ? g_screen_w : g_src_w;
        int ch = (caseidx == 3) ? g_screen_h : g_src_h;
        g_case_w = cw; g_case_h = ch;
        if (caseidx == 3) { srcrect.right = cw; srcrect.bottom = ch; }
    }

    IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);

    switch (caseidx) {
    case 0: srcfmt = D3DFMT_X8R8G8B8; dstfmt = g_dm.Format; break;
    case 1: srcfmt = FMT_YUY2;        dstfmt = g_dm.Format; break;
    case 2: srcfmt = D3DFMT_NV12;     dstfmt = g_dm.Format; break;
    case 3: srcfmt = D3DFMT_NV12;     dstfmt = g_dm.Format; break;   /* NV12 w rozmiarze ekranu, 1:1 */
    case 4: srcfmt = D3DFMT_NV12;     dstfmt = D3DFMT_X8R8G8B8; break;
    default: break;
    }

    hc = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_case_w, (UINT)g_case_h,
                                                      srcfmt, D3DPOOL_DEFAULT, &src, NULL);
    jhr("create_src", hc);
    jtext("src_format", fmt_name(srcfmt));
    jint("src_width", g_case_w);
    jint("src_height", g_case_h);
    jbool("src_created", SUCCEEDED(hc) && src != NULL);

    if (caseidx == 4) {
        HRESULT hr4 = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_screen_w, (UINT)g_screen_h,
                                                                   D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &rgb, NULL);
        jhr("create_dst_offscreen", hr4);
        dstsurf = rgb;
    } else {
        dstsurf = bb;
        jbool("dst_is_backbuffer", bb != NULL);
    }

    if (!src || !dstsurf) {
        jtext("skipped", "src or dst surface missing");
        if (bb) IDirect3DSurface9_Release(bb);
        if (src) IDirect3DSurface9_Release(src);
        if (rgb) IDirect3DSurface9_Release(rgb);
        elem_end("stretch");
        return;
    }

    /* KONTROLA PRZED WYWOLANIEM - to jest ta lekcja z crasha v1 */
    {
        HRESULT cv = IDirect3D9_CheckDeviceFormatConversion(g_d3d, D3DADAPTER_DEFAULT,
                                                            D3DDEVTYPE_HAL, srcfmt, dstfmt);
        jhr("check_format_conversion", cv);
        jbool("conversion_allowed", SUCCEEDED(cv));
        if (FAILED(cv)) {
            jtext("decision", "NIE wywolujemy StretchRect - CheckDeviceFormatConversion odmowil. "
                              "To samo musi robic klient produkcyjny, bo wywolanie wbrew "
                              "sterownikowi konczy sie 0xC0000005 (zob. v1).");
            if (bb) IDirect3DSurface9_Release(bb);
            IDirect3DSurface9_Release(src);
            if (rgb) IDirect3DSurface9_Release(rgb);
            elem_end("stretch");
            return;
        }
    }

    {
        HRESULT s1 = IDirect3DDevice9_StretchRect(g_dev, src, &srcrect, dstsurf,
                                                  (caseidx == 4) ? NULL : &dstrect, D3DTEXF_LINEAR);
        jhr("linear", s1);
        jbool("linear_ok", SUCCEEDED(s1));
        if (SUCCEEDED(s1)) {
            HRESULT s2 = IDirect3DDevice9_StretchRect(g_dev, src, &srcrect, dstsurf,
                                                      (caseidx == 4) ? NULL : &dstrect, D3DTEXF_POINT);
            jhr("point", s2);
            jbool("point_ok", SUCCEEDED(s2));
            IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
        }
    }
    hs = IDirect3DDevice9_TestCooperativeLevel(g_dev);
    jhr("test_cooperative_level_after", hs);

    if (bb) IDirect3DSurface9_Release(bb);
    IDirect3DSurface9_Release(src);
    if (rgb) IDirect3DSurface9_Release(rgb);
    elem_end("stretch");
}

/* ------------------------------------------------------------ etap: bench */

static void stage_bench(int which)
{
    /* which: 0 = RGB StretchRect+Present (bezpieczny), 1 = sam Present,
     *        2 = NV12 StretchRect+Present (ryzykowny), 3 = GDI */
    static const char *wnames[4] = {"stretch_present_rgb", "present_only",
                                    "stretch_present_nv12", "gdi_setdibits"};
    mark("stage_bench");
    elem_begin();
    jtext("bench", wnames[which & 3]);
    if (!device_alive()) { jtext("skipped", "device not alive"); elem_end("bench"); return; }

    if (which == 3) {
        HDC hdc = GetDC(g_hwnd);
        if (hdc) {
            BITMAPINFO bmi;
            void *bits = NULL;
            HBITMAP dib;
            memset(&bmi, 0, sizeof(bmi));
            bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth       = g_src_w;
            bmi.bmiHeader.biHeight      = -g_src_h;
            bmi.bmiHeader.biPlanes      = 1;
            bmi.bmiHeader.biBitCount    = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (dib && bits) {
                double *s = (double *)malloc(sizeof(double) * (size_t)g_iters);
                int i;
                memset(bits, 0x40, (size_t)g_src_w * (size_t)g_src_h * 4u);
                for (i = 0; i < 10; ++i)
                    SetDIBitsToDevice(hdc, 0, 0, (DWORD)g_src_w, (DWORD)g_src_h, 0, 0, 0,
                                      (UINT)g_src_h, bits, &bmi, DIB_RGB_COLORS);
                for (i = 0; i < g_iters; ++i) {
                    double t0 = now_ms();
                    SetDIBitsToDevice(hdc, 0, 0, (DWORD)g_src_w, (DWORD)g_src_h, 0, 0, 0,
                                      (UINT)g_src_h, bits, &bmi, DIB_RGB_COLORS);
                    if (s) s[i] = now_ms() - t0;
                }
                emit_timing("timing", s, g_iters, g_iters);
                free(s);
            } else {
                jtext("skipped", "CreateDIBSection failed");
            }
            if (dib) DeleteObject(dib);
            ReleaseDC(g_hwnd, hdc);
        }
        elem_end("bench");
        return;
    }

    if (which == 1) {
        double *s = (double *)malloc(sizeof(double) * (size_t)g_iters);
        int i;
        for (i = 0; i < 10; ++i) IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
        for (i = 0; i < g_iters; ++i) {
            double t0 = now_ms();
            IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
            if (s) s[i] = now_ms() - t0;
        }
        emit_timing("timing", s, g_iters, g_iters);
        free(s);
        elem_end("bench");
        return;
    }

    {
        D3DFORMAT fmt = (which == 2) ? D3DFMT_NV12 : D3DFMT_X8R8G8B8;
        IDirect3DSurface9 *src = NULL, *bb = NULL;
        RECT srcrect, dstrect;
        HRESULT hc;
        srcrect.left = 0; srcrect.top = 0; srcrect.right = g_src_w; srcrect.bottom = g_src_h;
        dstrect.left = 0; dstrect.top = 0;  dstrect.right = g_screen_w; dstrect.bottom = g_screen_h;

        IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        hc = IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, (UINT)g_src_w, (UINT)g_src_h,
                                                          fmt, D3DPOOL_DEFAULT, &src, NULL);
        jhr("create_src", hc);
        jtext("src_format", fmt_name(fmt));
        if (!src || !bb) {
            jtext("skipped", "src or backbuffer missing");
        } else if (which == 2 && !conv_allowed(fmt, g_dm.Format)) {
            jtext("skipped", "CheckDeviceFormatConversion odmowil NV12 -> backbuffer; "
                             "nie wywolujemy StretchRect (patrz v1 crash)");
        } else {
            double *s = (double *)malloc(sizeof(double) * (size_t)g_iters);
            int i, okc = 0;
            HRESULT firsterr = S_OK;
            for (i = 0; i < 10; ++i) {
                IDirect3DDevice9_StretchRect(g_dev, src, &srcrect, bb, &dstrect, D3DTEXF_LINEAR);
                IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
            }
            for (i = 0; i < g_iters; ++i) {
                double t0 = now_ms();
                HRESULT pr;
                IDirect3DDevice9_StretchRect(g_dev, src, &srcrect, bb, &dstrect, D3DTEXF_LINEAR);
                pr = IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
                if (s) s[i] = now_ms() - t0;
                if (SUCCEEDED(pr)) okc++;
                else if (firsterr == S_OK) firsterr = pr;
            }
            if (firsterr != S_OK) jhr("first_present_error", firsterr);
            jint("present_ok_count", okc);
            emit_timing("timing", s, g_iters, g_iters);
            free(s);
        }
        if (bb) IDirect3DSurface9_Release(bb);
        if (src) IDirect3DSurface9_Release(src);
    }
    elem_end("bench");
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    D3DPRESENT_PARAMETERS pp;
    const char *stage = "all";
    int caseidx = 2, which = 0, i;

    /* prosta, czytelna obsluga argumentow */
    {
        char outpath[1024];
        outpath[0] = 0;
        for (i = 1; i < argc; ++i) {
            if (!strncmp(argv[i], "--out=", 6)) {
                strncpy(outpath, argv[i] + 6, sizeof(outpath) - 1);
                outpath[sizeof(outpath) - 1] = 0;
            } else if (!strncmp(argv[i], "--stage=", 8)) {
                stage = argv[i] + 8;
            } else if (!strncmp(argv[i], "--case=", 7)) {
                caseidx = atoi(argv[i] + 7);
            } else if (!strncmp(argv[i], "--which=", 8)) {
                which = atoi(argv[i] + 8);
            } else if (!strncmp(argv[i], "--iters=", 8)) {
                g_iters = atoi(argv[i] + 8);
                if (g_iters < 1) g_iters = 1;
                if (g_iters > 5000) g_iters = 5000;
            }
        }
        if (!outpath[0]) strncpy(outpath, "T4_probe.json", sizeof(outpath) - 1);
        strncpy(g_outpath, outpath, sizeof(g_outpath) - 1);
        g_outpath[sizeof(g_outpath) - 1] = 0;
        {   /* sprawdz, ze dalo sie otworzyc do zapisu */
            FILE *probe_f = fopen(g_outpath, "wb");
            if (!probe_f) { fprintf(stderr, "[t4] cannot open %s\n", g_outpath); return 3; }
            fclose(probe_f);
        }
    }

    json_begin(stage, caseidx, which, g_iters);

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) {
        elem_begin();
        jtext("fatal", "Direct3DCreate9 returned NULL");
        elem_end("fatal");
        return 2;
    }
    memset(&g_dm, 0, sizeof(g_dm));
    if (FAILED(IDirect3D9_GetAdapterDisplayMode(g_d3d, D3DADAPTER_DEFAULT, &g_dm)) || g_dm.Width == 0) {
        g_dm.Width = 1280; g_dm.Height = 800; g_dm.RefreshRate = 60; g_dm.Format = D3DFMT_X8R8G8B8;
    }
    g_screen_w = (int)g_dm.Width;
    g_screen_h = (int)g_dm.Height;

    g_hwnd = make_window(g_screen_w, g_screen_h);
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth      = (UINT)g_screen_w;
    pp.BackBufferHeight     = (UINT)g_screen_h;
    pp.BackBufferFormat     = g_dm.Format;
    pp.BackBufferCount      = 1;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow        = g_hwnd;
    pp.Windowed             = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    if (g_hwnd) {
        g_behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
        g_hr_hwvp = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                            g_hwnd, g_behavior, &pp, &g_dev);
        if (FAILED(g_hr_hwvp) || !g_dev) {
            g_behavior = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED;
            g_hr_swvp = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                g_hwnd, g_behavior, &pp, &g_dev);
        }
    }

    if (!strcmp(stage, "all")) {
        stage_info();
        stage_formats();
        stage_device();
        stage_surfaces();
        for (i = 0; i <= 5; ++i) stage_updatesurf(i);
        for (i = 0; i <= 4; ++i) stage_stretch(i);
        for (i = 0; i <= 3; ++i) stage_bench(i);
    } else if (!strcmp(stage, "info"))       stage_info();
    else if (!strcmp(stage, "formats"))      stage_formats();
    else if (!strcmp(stage, "device"))       stage_device();
    else if (!strcmp(stage, "surfaces"))     stage_surfaces();
    else if (!strcmp(stage, "updatesurf"))   stage_updatesurf(caseidx);
    else if (!strcmp(stage, "stretch"))      stage_stretch(caseidx);
    else if (!strcmp(stage, "bench"))        stage_bench(which);
    else { elem_begin(); jtext("fatal", "unknown stage"); elem_end("fatal"); }

    if (g_dev) IDirect3DDevice9_Release(g_dev);
    if (g_d3d) IDirect3D9_Release(g_d3d);
    if (g_hwnd) DestroyWindow(g_hwnd);
    return 0;
}
