// ThemeHook3.dll - v5.4: EAT hooks + skip-only ring suppression + pale-tint fix
// - v5.0: EAT patch of provider dlls (cairo/gdi32/user32) -> late modules hooked instantly
// - v5.1: pale tints (min ch > 0.55) are backgrounds, not accents (F1 yellow banner fixed)
// - v5.2-5.3: duplicate-pair detection via per-thread ring history (pairs are not adjacent)
// - v5.4: HARD LESSON - any re-draw or pixel write lands in CACHED surfaces and poisons
//         the process permanently (doubling survived even full unload). Now SKIP-ONLY:
//         no deferred redraws, no surf-remap; unmatched dark text just recolored (safe)
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

static const int LOG_FIRST_N = 20;
static HMODULE g_hModule = NULL;
static CRITICAL_SECTION g_cs;
static FILE* g_log = NULL;

// ---------- IAT patch bookkeeping for clean unload ----------
#define MAX_PATCHES 4096
typedef struct { void** slot; void* orig; } PatchRec;
static PatchRec g_patches[MAX_PATCHES];
static int g_nPatches = 0;

// ---------- Originals ----------
typedef int (WINAPI *PFN_FillRect)(HDC, const RECT*, HBRUSH);
typedef COLORREF (WINAPI *PFN_SetBkColor)(HDC, COLORREF);
typedef COLORREF (WINAPI *PFN_SetTextColor)(HDC, COLORREF);
typedef BOOL (WINAPI *PFN_ExtTextOutW)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
typedef HBRUSH (WINAPI *PFN_CreateSolidBrush)(COLORREF);
typedef void (CDECL *PFN_cairo_rgb)(void*, double, double, double);
typedef void (CDECL *PFN_cairo_rgba)(void*, double, double, double, double);
typedef void* (CDECL *PFN_cairo_get_target)(void*);
typedef void* (CDECL *PFN_cairo_win32_surface_get_dc)(void*);
typedef int (CDECL *PFN_cairo_image_surface_get_width)(void*);
typedef int (CDECL *PFN_cairo_surface_get_type)(void*);
typedef void (CDECL *PFN_cairo_clip_extents)(void*, double*, double*, double*, double*);
typedef void (CDECL *PFN_cairo_show_glyphs)(void*, void*, int);
typedef void (CDECL *PFN_cairo_show_text)(void*, const char*);
typedef void (CDECL *PFN_cairo_destroy)(void*);
typedef void (CDECL *PFN_cairo_glyph_path)(void*, void*, int);
typedef void (CDECL *PFN_cairo_fill)(void*);
static PFN_cairo_glyph_path oCairoGlyphPath = NULL;
static PFN_cairo_fill oCairoFill = NULL;
typedef void* (CDECL *PFN_cairo_get_source)(void*);
typedef int (CDECL *PFN_cairo_pattern_get_rgba)(void*, double*, double*, double*, double*);
typedef void* (CDECL *PFN_cairo_pattern_create_rgb)(double, double, double);
typedef void* (CDECL *PFN_cairo_pattern_create_rgba)(double, double, double, double);
typedef void (CDECL *PFN_cairo_set_source)(void*, void*);
typedef void (CDECL *PFN_cairo_set_source_surface)(void*, void*, double, double);
static PFN_cairo_pattern_create_rgb oCairoPatCreateRgb = NULL;
static PFN_cairo_pattern_create_rgba oCairoPatCreateRgba = NULL;
static PFN_cairo_set_source oCairoSetSource = NULL;
static PFN_cairo_set_source_surface oCairoSetSourceSurface = NULL;
static PFN_cairo_get_target p_cairo_get_target = NULL;
static PFN_cairo_win32_surface_get_dc p_cairo_surface_get_dc = NULL;
static PFN_cairo_image_surface_get_width p_cairo_img_get_width = NULL;
static PFN_cairo_surface_get_type p_cairo_surf_get_type = NULL;
static PFN_cairo_clip_extents p_cairo_clip_extents = NULL;
static PFN_cairo_get_source p_cairo_get_source = NULL;
static PFN_cairo_pattern_get_rgba p_cairo_pattern_get_rgba = NULL;
static PFN_cairo_show_glyphs oCairoShowGlyphs = NULL;
static PFN_cairo_show_text oCairoShowText = NULL;
static PFN_cairo_destroy oCairoDestroy = NULL;
typedef DWORD (WINAPI *PFN_GetSysColor)(int);
typedef HBRUSH (WINAPI *PFN_GetSysColorBrush)(int);
typedef int (WINAPI *PFN_DrawTextW)(HDC, LPCWSTR, int, RECT*, UINT);
typedef int (WINAPI *PFN_DrawTextExW)(HDC, LPWSTR, int, RECT*, UINT, void*);
static PFN_DrawTextW oDrawTextW = NULL;
static PFN_DrawTextExW oDrawTextExW = NULL;

static PFN_FillRect oFillRect = NULL;
static PFN_SetBkColor oSetBkColor = NULL;
static PFN_SetTextColor oSetTextColor = NULL;
static PFN_ExtTextOutW oExtTextOutW = NULL;
static PFN_CreateSolidBrush oCreateSolidBrush = NULL;
static PFN_cairo_rgb oCairoRgb = NULL;
static PFN_cairo_rgba oCairoRgba = NULL;
static PFN_GetSysColor oGetSysColor = NULL;
static PFN_GetSysColorBrush oGetSysColorBrush = NULL;

static long nCairoRgb = 0, nCairoRgba = 0, nSetTextColor = 0, nCreateSolidBrush = 0,
            nFillRect = 0, nSetBkColor = 0, nGetSysColor = 0, nGetSysColorBrush = 0, nExtTextOut = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    if (g_log) {
        va_list ap; va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
}

// ---------- Explicit palette map (source: palette_dump.txt -> EDT Dark reference) ----------
typedef struct { COLORREF from; COLORREF to; } MapEntry;
static const MapEntry g_map[] = {
    { 0xFFFFFF, 0x1E1E1E },  // editor bg white      -> EDT editor bg
    { 0xF5F2DD, 0x333333 },  // 1C cream panel bg    -> EDT panel
    { 0xFCFAEB, 0x333333 },  // light cream          -> EDT panel
    { 0xDCD9C4, 0x2F2F2F },  // panel darker         -> EDT active tab
    { 0xD7D3BD, 0x2F2F2F },
    { 0xDBCDBF, 0x2F2F2F },  // GetSysColor idx3
    { 0xF2E4D7, 0x333333 },  // GetSysColor idx28
    { 0xB3AC86, 0x4B4B4B },  // olive border         -> EDT border
    { 0x9B8549, 0x393A3B },  // olive accent/MDI     -> EDT MDI bg
    { 0x413003, 0xD0D0D0 },  // dark olive text      -> EDT text
    { 0x000000, 0xD0D0D0 },  // black text           -> EDT text
    { 0x4D4D4D, 0xD0D0D0 },  // gray text            -> EDT text
    { 0x333333, 0xD0D0D0 },  // (as SOURCE: dark gray text -> EDT text)
    { 0x737373, 0xB5B5B5 },  // secondary text       -> EDT secondary
    { 0x696969, 0xB5B5B5 },
    { 0xB2B2B2, 0x747474 },  // disabled gray        -> EDT dimmed
    { 0x8D8D8D, 0x9A9A9A },
    { 0xA7A7A7, 0x747474 },
    { 0xA0A0A0, 0x7A7A7A },
    { 0xCCCCCC, 0x747474 },
    { 0xE3E3E3, 0x393A3B },
    { 0xF1F1F1, 0x282828 },
    { 0xB2CCE6, 0x303062 },  // light selection blue -> EDT selection
    { 0x6F98CF, 0x729DBA },  // blue accent          -> EDT blue
    // 1C syntax colors (measured in code modules) -> EDT code accents
    { 0xFF0000, 0xFF785A },  // 1C keyword red       -> EDT keyword
    { 0x0000FF, 0x729DBA },  // 1C blue (numbers)    -> EDT blue
    { 0x008000, 0x8DB5A1 },  // 1C green comment     -> EDT green
    { 0x963200, 0xB58756 },  // 1C brown string      -> EDT string
    { 0x800080, 0xB58756 },  // 1C purple            -> EDT brown-orange
    { 0x008080, 0x5D9DA5 },  // 1C teal              -> EDT teal
    { 0x00FFFF, 0x5D9DA5 },  // 1C cyan marker       -> EDT teal
    { 0x4C4C4C, 0xB5B5B5 },  // 1C gray (operators?) -> EDT secondary
    { 0x594304, 0xB5B5B5 },  // 1C dark olive text   -> EDT secondary
    { 0x222222, 0xD0D0D0 },  // near-black text      -> EDT text
    { 0xEBE7CD, 0x2F2F2F },  // cream shade          -> EDT active tab
    { 0xFAF9F3, 0x333333 },  // near-white cream     -> EDT panel
    { 0xCCC6AD, 0x2F2F2F },  // cream dark           -> EDT active tab
    { 0xE6E6E6, 0x393A3B },  // light gray fill      -> EDT MDI
    { 0x536AC2, 0x729DBA },  // help hyperlink blue  -> EDT blue
    { 0x0000EE, 0x729DBA },  // HTML link blue       -> EDT blue
    { 0x0000CD, 0x729DBA },  // HTML link medium blue-> EDT blue
    { 0x551A8B, 0xB58756 },  // HTML visited link    -> EDT brown-orange
    { 0x033041, 0x5D9DA5 },  // dark teal accent     -> EDT teal
};
static const int g_mapSize = sizeof(g_map) / sizeof(g_map[0]);

static int color_dist(int r1, int g1, int b1, int r2, int g2, int b2)
{
    int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr * dr + dg * dg + db * db;
}

// ---------- HSL fallback: invert lightness, keep hue ----------
static void rgb2hsl(int r, int g, int b, double* h, double* s, double* l)
{
    double R = r / 255.0, G = g / 255.0, B = b / 255.0;
    double mx = max(R, max(G, B)), mn = min(R, min(G, B));
    *l = (mx + mn) / 2.0;
    if (mx == mn) { *h = 0; *s = 0; return; }
    double d = mx - mn;
    *s = (*l > 0.5) ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == R)      *h = (G - B) / d + (G < B ? 6.0 : 0.0);
    else if (mx == G) *h = (B - R) / d + 2.0;
    else              *h = (R - G) / d + 4.0;
    *h /= 6.0;
}
static double hue2rgb(double p, double q, double t)
{
    if (t < 0) t += 1; if (t > 1) t -= 1;
    if (t < 1.0/6) return p + (q - p) * 6 * t;
    if (t < 1.0/2) return q;
    if (t < 2.0/3) return p + (q - p) * (2.0/3 - t) * 6;
    return p;
}
static void hsl2rgb(double h, double s, double l, int* r, int* g, int* b)
{
    double R, G, B;
    if (s == 0) { R = G = B = l; }
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        R = hue2rgb(p, q, h + 1.0/3);
        G = hue2rgb(p, q, h);
        B = hue2rgb(p, q, h - 1.0/3);
    }
    *r = (int)(R * 255 + 0.5); *g = (int)(G * 255 + 0.5); *b = (int)(B * 255 + 0.5);
}

static COLORREF MapColor(COLORREF c)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    // exact / near match in explicit map (tolerance ~6 per channel)
    int best = -1, bestd = 3 * 6 * 6 + 1;
    for (int i = 0; i < g_mapSize; i++) {
        int d = color_dist(r, g, b,
            GetRValue(g_map[i].from), GetGValue(g_map[i].from), GetBValue(g_map[i].from));
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best >= 0) return g_map[best].to;
    // HSL fallback (EDT levels: bg ~0.12, text ~0.82)
    double h, s, l;
    rgb2hsl(r, g, b, &h, &s, &l);
    double mx = max(r, max(g, b)) / 255.0, mn = min(r, min(g, b)) / 255.0;
    double chroma = mx - mn;
    double nl, ns;
    if (chroma > 0.15 && s > 0.35 && mn <= 0.55) {
        // truly saturated accent: keep hue, make it readable on dark bg
        nl = 0.65;
        ns = s * 0.85;
    } else {
        // near-neutral: invert lightness; white -> 0.12 (#1F), black -> 0.82 (#D1)
        nl = 0.12 + (1.0 - l) * 0.70;
        ns = s * 0.1;   // desaturate creams so they don't go olive
    }
    int nr, ng, nb;
    hsl2rgb(h, ns, nl, &nr, &ng, &nb);
    return RGB(nr, ng, nb);
}

// true if color already equals one of map targets (prevents double-mapping in FillRect)
static bool IsMappedTarget(COLORREF c)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    for (int i = 0; i < g_mapSize; i++) {
        int d = color_dist(r, g, b,
            GetRValue(g_map[i].to), GetGValue(g_map[i].to), GetBValue(g_map[i].to));
        if (d <= 3 * 4 * 4) return true;
    }
    return false;
}

// ---------- IAT hook engine ----------
static int PatchIAT(HMODULE hMod, const char* dllName, const char* fnName, void* newFn, void** pOld)
{
    int patched = 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    __try {
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        IMAGE_DATA_DIRECTORY impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (impDir.Size == 0) return 0;
        PIMAGE_IMPORT_DESCRIPTOR desc = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)hMod + impDir.VirtualAddress);
        for (; desc->Name; desc++) {
            const char* modName = (const char*)((BYTE*)hMod + desc->Name);
            if (_stricmp(modName, dllName) != 0) continue;
            PIMAGE_THUNK_DATA thunk = (PIMAGE_THUNK_DATA)((BYTE*)hMod + desc->FirstThunk);
            PIMAGE_THUNK_DATA orig  = (PIMAGE_THUNK_DATA)((BYTE*)hMod +
                (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
            for (; thunk->u1.Function; thunk++, orig++) {
                if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
                PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)((BYTE*)hMod + orig->u1.AddressOfData);
                if (strcmp((const char*)ibn->Name, fnName) != 0) continue;
                if ((void*)thunk->u1.Function == newFn) { patched++; continue; }  // already hooked (re-scan safe)
                DWORD oldProt;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    if (pOld && !*pOld) *pOld = (void*)thunk->u1.Function;
                    if (g_nPatches < MAX_PATCHES) {
                        g_patches[g_nPatches].slot = (void**)&thunk->u1.Function;
                        g_patches[g_nPatches].orig = (void*)thunk->u1.Function;
                        g_nPatches++;
                    }
                    thunk->u1.Function = (DWORD_PTR)newFn;
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                    patched++;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return patched;
}

// ---------- EAT patch engine (v5 hypothesis A) ----------
// Instead of patching every module's IAT (and re-scanning for late-loaded DLLs),
// patch the Export Address Table of the PROVIDING dll (cairo.dll / gdi32.dll / user32.dll)
// once. Any module that resolves the import afterwards (IAT bind, delay-load,
// GetProcAddress) automatically receives the hooked address.
// EAT entries are RVAs relative to the exporting module base; storing an RVA that
// points outside the module is legal (GetProcAddress just computes base+RVA),
// which lets us point the export at our hook function in ThemeHook3.dll.
static int PatchEAT(HMODULE hMod, const char* fnName, void* newFn, void** pOld)
{
    if (!hMod) return 0;
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    int patched = 0;
    __try {
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        IMAGE_DATA_DIRECTORY expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (expDir.Size == 0) return 0;
        PIMAGE_EXPORT_DIRECTORY exp = (PIMAGE_EXPORT_DIRECTORY)((BYTE*)hMod + expDir.VirtualAddress);
        DWORD* names = (DWORD*)((BYTE*)hMod + exp->AddressOfNames);
        WORD*  ords  = (WORD*)((BYTE*)hMod + exp->AddressOfNameOrdinals);
        DWORD* funcs = (DWORD*)((BYTE*)hMod + exp->AddressOfFunctions);
        for (DWORD i = 0; i < exp->NumberOfNames; i++) {
            const char* name = (const char*)((BYTE*)hMod + names[i]);
            if (strcmp(name, fnName) != 0) continue;
            DWORD idx = ords[i];
            DWORD newRva = (DWORD)((BYTE*)newFn - (BYTE*)hMod);
            if (funcs[idx] == newRva) { patched++; break; }  // already EAT-hooked
            DWORD oldProt;
            if (VirtualProtect(&funcs[idx], sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
                // capture the true original from the EAT (most reliable source)
                if (pOld && !*pOld) *pOld = (void*)((BYTE*)hMod + funcs[idx]);
                if (g_nPatches < MAX_PATCHES) {
                    g_patches[g_nPatches].slot = (void**)&funcs[idx];
                    g_patches[g_nPatches].orig = (void*)(DWORD_PTR)funcs[idx];
                    g_nPatches++;
                }
                funcs[idx] = newRva;
                VirtualProtect(&funcs[idx], sizeof(DWORD), oldProt, &oldProt);
                patched++;
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return patched;
}

static void HookAll(const char* dll, const char* fn, void* newFn, void** pOld, const char* label)
{
    int total = 0, eat = 0;
    // v5: patch the provider's EAT once — late-loaded modules get the hook for free
    HMODULE hProv = GetModuleHandleA(dll);
    if (hProv) eat = PatchEAT(hProv, fn, newFn, pOld);
    HMODULE mods[1024]; DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        if (count > 1024) count = 1024;
        for (int i = 0; i < count; i++) total += PatchIAT(mods[i], dll, fn, newFn, pOld);
    }
    Log("[hook] %s!%s (%s): EAT %d, patched %d IAT entries\n", dll, fn, label, eat, total);
    if (pOld && !*pOld) {
        HMODULE h = GetModuleHandleA(dll);
        if (h) *pOld = (void*)GetProcAddress(h, fn);
    }
}

// ---------- Hooked functions (pure transforms, no locks) ----------
// luminance 0..255; used by GDI text ghost-fix (emboss shadow pass)
static int Lum(COLORREF c)
{
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}
// If mapped text color is too dark for our dark backgrounds (emboss/shadow pass),
// force it to the standard text color so both passes coincide.
static COLORREF FixupGdiTextColor(COLORREF mapped)
{
    if (Lum(mapped) < 90) return 0xD0D0D0;
    return mapped;
}
// Role detection for near-white backgrounds: tree panel (narrow window) -> #333333
static long nCtxDc = 0, nCtxNoDc = 0;
static COLORREF MapBgWithContext(COLORREF mapped, COLORREF orig, void* cr)
{
    // only interested when mapping white-ish to editor bg
    if (mapped != 0x1E1E1E) return mapped;
    if (!p_cairo_get_target || !p_cairo_surface_get_dc) return mapped;
    void* surf = p_cairo_get_target(cr);
    if (!surf) { InterlockedIncrement(&nCtxNoDc); return mapped; }
    HDC dc = (HDC)p_cairo_surface_get_dc(surf);
    if (!dc) {
        InterlockedIncrement(&nCtxNoDc);
        // image surface (shared backbuffer): use clip extents as role heuristic.
        // Tree = narrow region at left edge of main window.
        if (p_cairo_clip_extents) {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            p_cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
            double rw = x2 - x1;
            if (nCtxNoDc <= LOG_FIRST_N)
                Log("[ctx] white fill clip x1=%.0f w=%.0f h=%.0f\n", x1, rw, y2 - y1);
            if (rw > 0 && rw < 700 && x1 < 500)
                return 0x333333;  // narrow left panel (tree) -> EDT panel
        }
        return mapped;
    }
    InterlockedIncrement(&nCtxDc);
    HWND w = WindowFromDC(dc);
    if (!w) return mapped;
    RECT rc; GetWindowRect(w, &rc);
    int width = rc.right - rc.left;
    char cls[64] = {0};
    GetClassNameA(w, cls, 63);
    if (nCtxDc <= LOG_FIRST_N)
        Log("[ctx] white on hwnd=%p class=%s w=%d\n", w, cls, width);
    if (width > 0 && width < 700)
        return 0x333333;  // narrow panel (tree) -> EDT panel
    return mapped;
}

static void CDECL hCairoRgb(void* cr, double r, double g, double b)
{
    InterlockedIncrement(&nCairoRgb);
    COLORREF c = RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
    COLORREF m = MapColor(c);
    m = MapBgWithContext(m, c, cr);
    if (m != c && nCairoRgb <= LOG_FIRST_N)
        Log("[cairo_rgb #%ld] %06X -> %06X\n", nCairoRgb, c, m);
    oCairoRgb(cr, GetRValue(m) / 255.0, GetGValue(m) / 255.0, GetBValue(m) / 255.0);
}
static void CDECL hCairoRgba(void* cr, double r, double g, double b, double a)
{
    InterlockedIncrement(&nCairoRgba);
    COLORREF c = RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
    COLORREF m = MapColor(c);
    m = MapBgWithContext(m, c, cr);
    if (m != c && nCairoRgba <= LOG_FIRST_N)
        Log("[cairo_rgba #%ld] %06X -> %06X\n", nCairoRgba, c, m);
    oCairoRgba(cr, GetRValue(m) / 255.0, GetGValue(m) / 255.0, GetBValue(m) / 255.0, a);
}
static COLORREF MapTextColor(COLORREF c)
{
    if (IsMappedTarget(c)) return c;  // already theme color: don't dim it again
    return FixupGdiTextColor(MapColor(c));
}
static COLORREF WINAPI hSetTextColor(HDC hdc, COLORREF c)
{
    InterlockedIncrement(&nSetTextColor);
    COLORREF m = MapTextColor(c);
    if (m != c && nSetTextColor <= LOG_FIRST_N)
        Log("[SetTextColor #%ld] %06X -> %06X\n", nSetTextColor, c, m);
    return oSetTextColor(hdc, m);
}
static COLORREF WINAPI hSetBkColor(HDC hdc, COLORREF c)
{
    InterlockedIncrement(&nSetBkColor);
    COLORREF m = MapColor(c);
    return oSetBkColor(hdc, m);
}
static HBRUSH WINAPI hCreateSolidBrush(COLORREF c)
{
    InterlockedIncrement(&nCreateSolidBrush);
    COLORREF m = MapColor(c);
    if (m != c && nCreateSolidBrush <= LOG_FIRST_N)
        Log("[CreateSolidBrush #%ld] %06X -> %06X\n", nCreateSolidBrush, c, m);
    return oCreateSolidBrush(m);
}
static int WINAPI hFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
{
    InterlockedIncrement(&nFillRect);
    LOGBRUSH lb;
    if ((DWORD_PTR)hbr >= 0x10000 &&
        GetObjectW(hbr, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID &&
        !IsMappedTarget(lb.lbColor)) {
        COLORREF m = MapColor(lb.lbColor);
        if (m != lb.lbColor) {
            if (nFillRect <= LOG_FIRST_N)
                Log("[FillRect #%ld] %06X -> %06X\n", nFillRect, lb.lbColor, m);
            HBRUSH tmp = CreateSolidBrush(m);
            int r = oFillRect(hdc, lprc, tmp);
            DeleteObject(tmp);
            return r;
        }
    }
    return oFillRect(hdc, lprc, hbr);
}
// ---------- v5.3: GDI ring-history duplicate suppression (menu emboss/bold) ----------
#define GRING_SIZE 128
typedef struct { WCHAR s[64]; int len; int x, y; COLORREF c; } GdiRec;
static __declspec(thread) GdiRec t_gring[GRING_SIZE];
static __declspec(thread) int t_ghead = 0;
static long nGdiDup = 0;
static int GdiDup(const WCHAR* s, int len, int x, int y, COLORREF m)
{
    if (!s || len <= 0 || len > 63) return 0;
    for (int i = 0; i < GRING_SIZE; i++) {
        GdiRec* r = &t_gring[i];
        if (r->len != len || r->c != m) continue;
        int off = abs(x - r->x) + abs(y - r->y);
        if (off < 1 || off > 4) continue;   // same-position repaints are never eaten
        if (wcsncmp(s, r->s, len) == 0) return 1;
    }
    return 0;
}
static void GdiRemember(const WCHAR* s, int len, int x, int y, COLORREF m)
{
    if (!s || len <= 0 || len > 63) return;
    GdiRec* r = &t_gring[t_ghead];
    wcsncpy(r->s, s, len);
    r->len = len; r->x = x; r->y = y; r->c = m;
    t_ghead = (t_ghead + 1) % GRING_SIZE;
}

static BOOL WINAPI hExtTextOutW(HDC hdc, int x, int y, UINT opt, const RECT* rc, LPCWSTR str, UINT len, const INT* dx)
{
    long n = InterlockedIncrement(&nExtTextOut);
    COLORREF tc = GetTextColor(hdc);
    COLORREF m = MapTextColor(tc);
    if (n <= 60) {
        Log("[eto #%ld] x=%d y=%d len=%u color=%06X -> %06X\n", n, x, y, len, tc, m);
    }
    if (GdiDup(str, (int)len, x, y, m)) {
        InterlockedIncrement(&nGdiDup);
        if (nGdiDup <= LOG_FIRST_N)
            Log("[gdi-dup #%ld] eto len=%u at %d,%d color=%06X (skipped)\n", nGdiDup, len, x, y, m);
        return TRUE;
    }
    GdiRemember(str, (int)len, x, y, m);
    if (m != tc) {
        if (oSetTextColor) oSetTextColor(hdc, m); else SetTextColor(hdc, m);
        BOOL r = oExtTextOutW(hdc, x, y, opt, rc, str, len, dx);
        if (oSetTextColor) oSetTextColor(hdc, tc); else SetTextColor(hdc, tc);
        return r;
    }
    return oExtTextOutW(hdc, x, y, opt, rc, str, len, dx);
}

// ---------- Text-drawing hooks: kill the emboss ghost ----------
// 1C help renders text twice with 1px offset (black + white emboss passes).
// Our bg mapping turns the white pass into editor-bg color => dark ghost on dark bg.
// Fix: right before glyphs are drawn, if current solid source is dark (L<0.25),
// re-point it to light text color. Both passes end up identical => crisp text.
static long nShowGlyphs = 0, nGhostFixed = 0;
static void FixupTextColor(void* cr)
{
    if (!p_cairo_get_source || !p_cairo_pattern_get_rgba || !oCairoRgb) return;
    void* pat = p_cairo_get_source(cr);
    if (!pat) return;
    double r = 0, g = 0, b = 0, a = 0;
    if (p_cairo_pattern_get_rgba(pat, &r, &g, &b, &a) != 0) return;  // not a solid color
    double L = 0.299 * r + 0.587 * g + 0.114 * b;
    if (L < 0.35) {
        InterlockedIncrement(&nGhostFixed);
        if (nGhostFixed <= LOG_FIRST_N)
            Log("[ghost-fix #%ld] text color %.2f,%.2f,%.2f -> #D0D0D0\n", nGhostFixed, r, g, b);
        oCairoRgb(cr, 0xD0 / 255.0, 0xD0 / 255.0, 0xD0 / 255.0);
    }
}

// ---------- v5.4: ring-history suppression, SKIP-ONLY (no re-draws, no pixel writes) ----------
// HARD LESSON (live experiment): any RE-DRAW into a cairo context can land in a
// CACHED surface (WebKit/1C text caches). Doubled content baked into a cache
// survives DLL unload and even theme removal - the process stays "poisoned"
// until restart (v5.1's 3477 deferred redraws did exactly this).
// Therefore v5.4 NEVER draws anything extra:
//  - emboss shadow AFTER main: confirmed via 256-run light ring -> skipped;
//  - unmatched dark draw (maybe shadow-first, maybe legit white text): NOT skipped,
//    just recolored to #D0D0D0 (legacy safe behavior, zero risk of losing text);
//  - synthetic-bold duplicate (same glyphs+color, offset 0.25..3px): skipped;
//  - surf-remap (in-place pixel modification of cached surfaces) DISABLED:
//    same cache-poisoning risk class, 0 hits in v4.8+ anyway.
typedef struct { unsigned long index; double x; double y; } CairoGlyph;  // x86 layout, 24 bytes
#define LRING_SIZE 256
typedef struct {
    int num; unsigned long idx[8];
    double x, y, r, g, b;
} LightRec;
static __declspec(thread) LightRec t_lring[LRING_SIZE];
static __declspec(thread) int t_lhead = 0;
static long nShadowSkip = 0, nDupSkip = 0;

static int SrcRGB(void* cr, double* r, double* g, double* b)  // 1 if solid color
{
    if (!p_cairo_get_source || !p_cairo_pattern_get_rgba) return 0;
    void* pat = p_cairo_get_source(cr);
    if (!pat) return 0;
    double a = 0;
    return p_cairo_pattern_get_rgba(pat, r, g, b, &a) == 0;
}
static int GlyphRecMatch(const CairoGlyph* g, int num, const LightRec* rec)
{
    if (num != rec->num || num <= 0) return 0;
    double off = fabs(g[0].x - rec->x) + fabs(g[0].y - rec->y);
    if (off < 0.25 || off > 3.0) return 0;  // 0 = same-position repaint: never a pair
    int n = num < 8 ? num : 8;
    for (int i = 0; i < n; i++)
        if (g[i].index != rec->idx[i]) return 0;
    return 1;
}
// earlier light run with same glyphs (any color) => we are the shadow of that pair
static int LightRingHasPair(const CairoGlyph* g, int num)
{
    for (int i = 0; i < LRING_SIZE; i++)
        if (t_lring[i].num > 0 && GlyphRecMatch(g, num, &t_lring[i])) return 1;
    return 0;
}
// earlier light run with same glyphs AND same color => synthetic-bold duplicate
static int LightRingHasDup(const CairoGlyph* g, int num, double r, double gg, double b)
{
    for (int i = 0; i < LRING_SIZE; i++) {
        LightRec* rec = &t_lring[i];
        if (rec->num <= 0 || !GlyphRecMatch(g, num, rec)) continue;
        double d = fabs(r - rec->r) + fabs(gg - rec->g) + fabs(b - rec->b);
        if (d < 0.05) return 1;
    }
    return 0;
}
static void LightRingPush(const CairoGlyph* g, int num, double r, double gg, double b)
{
    LightRec* rec = &t_lring[t_lhead];
    rec->num = num;
    int n = num < 8 ? num : 8;
    for (int i = 0; i < n; i++) rec->idx[i] = g[i].index;
    rec->x = g[0].x; rec->y = g[0].y;
    rec->r = r; rec->g = gg; rec->b = b;
    t_lhead = (t_lhead + 1) % LRING_SIZE;
}

static void CDECL hCairoShowGlyphs(void* cr, void* glyphs, int num)
{
    InterlockedIncrement(&nShowGlyphs);
    CairoGlyph* g = (CairoGlyph*)glyphs;
    double rr = 0, gg = 0, bb = 0;
    int solid = SrcRGB(cr, &rr, &gg, &bb);
    if (solid && g && num > 0 && num <= 256) {
        double L = 0.299 * rr + 0.587 * gg + 0.114 * bb;
        if (L < 0.35) {
            // dark at glyph time = original light color. Skip ONLY when the pair
            // is confirmed by an earlier light run (shadow after main). Otherwise
            // fall through to the safe recolor - never defer, never redraw.
            if (LightRingHasPair(g, num)) {
                InterlockedIncrement(&nShadowSkip);
                if (nShadowSkip <= LOG_FIRST_N)
                    Log("[shadow-skip #%ld] num=%d at %.0f,%.0f\n", nShadowSkip, num, g[0].x, g[0].y);
                return;
            }
        } else {
            // light draw: same glyphs+color at 0.25..3px = synthetic-bold duplicate
            if (LightRingHasDup(g, num, rr, gg, bb)) {
                InterlockedIncrement(&nDupSkip);
                if (nDupSkip <= LOG_FIRST_N)
                    Log("[dup-skip #%ld] num=%d at %.0f,%.0f (synthetic bold)\n", nDupSkip, num, g[0].x, g[0].y);
                return;
            }
            LightRingPush(g, num, rr, gg, bb);
            oCairoShowGlyphs(cr, glyphs, num);
            return;
        }
    }
    // safe legacy path: dark text color -> #D0D0D0, draw normally
    FixupTextColor(cr);
    oCairoShowGlyphs(cr, glyphs, num);
}
static void CDECL hCairoDestroy(void* cr)
{
    oCairoDestroy(cr);
}
static void CDECL hCairoShowText(void* cr, const char* utf8)
{
    FixupTextColor(cr);
    oCairoShowText(cr, utf8);
}

// glyph_path + fill path: some renderers draw text as filled glyph outlines
static __declspec(thread) int t_glyphPathPending = 0;
static long nGlyphPath = 0, nFillAfterGlyph = 0;
static void CDECL hCairoGlyphPath(void* cr, void* glyphs, int num)
{
    InterlockedIncrement(&nGlyphPath);
    t_glyphPathPending = 1;
    oCairoGlyphPath(cr, glyphs, num);
}
static void CDECL hCairoFill(void* cr)
{
    if (t_glyphPathPending) {
        t_glyphPathPending = 0;
        InterlockedIncrement(&nFillAfterGlyph);
        FixupTextColor(cr);
    }
    oCairoFill(cr);
}

// ---------- Pattern-level hooks: catch colors latched in cached patterns ----------
static long nPatCreate = 0, nSetSource = 0;
static void* CDECL hPatCreateRgb(double r, double g, double b)
{
    InterlockedIncrement(&nPatCreate);
    COLORREF c = RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
    COLORREF m = IsMappedTarget(c) ? c : MapColor(c);
    if (m != c && nPatCreate <= LOG_FIRST_N)
        Log("[pat_rgb #%ld] %06X -> %06X\n", nPatCreate, c, m);
    return oCairoPatCreateRgb(GetRValue(m) / 255.0, GetGValue(m) / 255.0, GetBValue(m) / 255.0);
}
static void* CDECL hPatCreateRgba(double r, double g, double b, double a)
{
    COLORREF c = RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
    COLORREF m = IsMappedTarget(c) ? c : MapColor(c);
    return oCairoPatCreateRgba(GetRValue(m) / 255.0, GetGValue(m) / 255.0, GetBValue(m) / 255.0, a);
}
static void CDECL hCairoSetSource(void* cr, void* pattern)
{
    InterlockedIncrement(&nSetSource);
    if (p_cairo_pattern_get_rgba && pattern) {
        double r = 0, g = 0, b = 0, a = 0;
        if (p_cairo_pattern_get_rgba(pattern, &r, &g, &b, &a) == 0) {
            COLORREF c = RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
            if (!IsMappedTarget(c)) {
                COLORREF m = MapColor(c);
                if (m != c) {
                    if (nSetSource <= LOG_FIRST_N)
                        Log("[set_source #%ld] pattern %06X -> %06X\n", nSetSource, c, m);
                    oCairoRgba(cr, GetRValue(m) / 255.0, GetGValue(m) / 255.0, GetBValue(m) / 255.0, a);
                    return;
                }
            }
        }
    }
    oCairoSetSource(cr, pattern);
}

// ---------- set_source_surface: remap pixels of cached text bitmaps ----------
// Headings are rendered ONCE into a cached image surface (white emboss + black main,
// premultiplied ARGB32) and then blitted via cairo_set_source_surface, bypassing
// all color hooks. For small surfaces we remap pixels in place (once, marked by a
// sentinel scan): near-white AND near-black neutral pixels both -> text color.
static PFN_cairo_image_surface_get_width p_img_w = NULL;
typedef int (CDECL *PFN_cairo_image_surface_get_height)(void*);
typedef int (CDECL *PFN_cairo_image_surface_get_format)(void*);
typedef int (CDECL *PFN_cairo_image_surface_get_stride)(void*);
typedef unsigned char* (CDECL *PFN_cairo_image_surface_get_data)(void*);
typedef void (CDECL *PFN_cairo_surface_mark_dirty)(void*);
typedef void (CDECL *PFN_cairo_surface_flush)(void*);
static PFN_cairo_image_surface_get_height p_img_h = NULL;
static PFN_cairo_image_surface_get_format p_img_fmt = NULL;
static PFN_cairo_image_surface_get_stride p_img_stride = NULL;
static PFN_cairo_image_surface_get_data p_img_data = NULL;
static PFN_cairo_surface_mark_dirty p_mark_dirty = NULL;
static PFN_cairo_surface_flush p_surf_flush = NULL;
static long nSrcSurf = 0, nSurfRemapped = 0;

static void CDECL hCairoSetSourceSurface(void* cr, void* surf, double x, double y)
{
    // v5.4: pixel remapping of cached surfaces DISABLED. In-place modification of
    // cache surfaces is permanent cache poisoning (survives unload); 0 hits in v4.8+.
    InterlockedIncrement(&nSrcSurf);
    oCairoSetSourceSurface(cr, surf, x, y);
}

static long nDrawText = 0;
static int WINAPI hDrawTextW(HDC hdc, LPCWSTR str, int len, RECT* rc, UINT fmt)
{
    InterlockedIncrement(&nDrawText);
    COLORREF tc = GetTextColor(hdc);
    COLORREF m = MapTextColor(tc);
    int n = (len < 0 && str) ? (int)wcsnlen(str, 64) : len;
    int x = rc ? rc->left : 0, y = rc ? rc->top : 0;
    if (GdiDup(str, n, x, y, m)) {
        InterlockedIncrement(&nGdiDup);
        return 12;
    }
    GdiRemember(str, n, x, y, m);
    if (m != tc) {
        if (nDrawText <= LOG_FIRST_N)
            Log("[DrawTextW #%ld] color %06X -> %06X\n", nDrawText, tc, m);
        if (oSetTextColor) oSetTextColor(hdc, m); else SetTextColor(hdc, m);
        int r = oDrawTextW(hdc, str, len, rc, fmt);
        if (oSetTextColor) oSetTextColor(hdc, tc); else SetTextColor(hdc, tc);
        return r;
    }
    return oDrawTextW(hdc, str, len, rc, fmt);
}
static int WINAPI hDrawTextExW(HDC hdc, LPWSTR str, int len, RECT* rc, UINT fmt, void* params)
{
    COLORREF tc = GetTextColor(hdc);
    COLORREF m = MapTextColor(tc);
    int n = (len < 0 && str) ? (int)wcsnlen(str, 64) : len;
    int x = rc ? rc->left : 0, y = rc ? rc->top : 0;
    if (GdiDup(str, n, x, y, m)) {
        InterlockedIncrement(&nGdiDup);
        return 12;
    }
    GdiRemember(str, n, x, y, m);
    if (m != tc) {
        if (oSetTextColor) oSetTextColor(hdc, m); else SetTextColor(hdc, m);
        int r = oDrawTextExW(hdc, str, len, rc, fmt, params);
        if (oSetTextColor) oSetTextColor(hdc, tc); else SetTextColor(hdc, tc);
        return r;
    }
    return oDrawTextExW(hdc, str, len, rc, fmt, params);
}

// ---------- GetSysColor: role-based EDT mapping ----------
static COLORREF SysColorEDT(int idx, COLORREF c)
{
    switch (idx) {
    case 5:  return 0x333333;  // COLOR_WINDOW (tree bg)      -> EDT panel
    case 12: return 0x1E1E1E;  // COLOR_APPWORKSPACE          -> EDT editor bg
    case 13: return 0x303062;  // COLOR_HIGHLIGHT             -> EDT selection
    case 14: return 0xE8E8E8;  // COLOR_HIGHLIGHTTEXT         -> light on selection
    case 15: return 0x2F2F2F;  // COLOR_BTNFACE               -> EDT active tab
    case 16: return 0x282828;  // COLOR_BTNSHADOW             -> EDT dark area
    case 8:  return 0xD0D0D0;  // COLOR_WINDOWTEXT            -> EDT text
    case 18: return 0xD0D0D0;  // COLOR_BTNTEXT               -> EDT text
    default: return MapColor(c);
    }
}

static DWORD WINAPI hGetSysColor(int idx)
{
    InterlockedIncrement(&nGetSysColor);
    COLORREF c = oGetSysColor(idx);
    COLORREF m = SysColorEDT(idx, c);
    if (m != c && nGetSysColor <= LOG_FIRST_N)
        Log("[GetSysColor #%ld] idx=%d %06X -> %06X\n", nGetSysColor, idx, c, m);
    return m;
}

// GetSysColorBrush: only safe indices, pre-cached brushes; everything else -> original
static HBRUSH g_brCache[19];  // indexed by color idx, NULL = use original
static HBRUSH WINAPI hGetSysColorBrush(int idx)
{
    InterlockedIncrement(&nGetSysColorBrush);
    if (idx >= 0 && idx <= 18 && g_brCache[idx]) {
        if (nGetSysColorBrush <= LOG_FIRST_N)
            Log("[GetSysColorBrush #%ld] idx=%d -> cached dark brush\n", nGetSysColorBrush, idx);
        return g_brCache[idx];
    }
    return oGetSysColorBrush(idx);
}

// ---------- Hook table (re-scannable: new modules loaded after injection get patched too) ----------
typedef struct { const char* dll; const char* fn; void* newFn; void** pOld; const char* label; } HookDef;
static const HookDef g_hooks[] = {
    { "cairo.dll", "cairo_set_source_rgb",      (void*)hCairoRgb,            (void**)&oCairoRgb,            "cairo-fill" },
    { "cairo.dll", "cairo_set_source_rgba",     (void*)hCairoRgba,           (void**)&oCairoRgba,           "cairo-filla" },
    { "cairo.dll", "cairo_show_glyphs",         (void*)hCairoShowGlyphs,     (void**)&oCairoShowGlyphs,     "glyphs" },
    { "cairo.dll", "cairo_show_text",           (void*)hCairoShowText,       (void**)&oCairoShowText,       "text-glyphs" },
    { "cairo.dll", "cairo_glyph_path",          (void*)hCairoGlyphPath,      (void**)&oCairoGlyphPath,      "glyph-path" },
    { "cairo.dll", "cairo_fill",                (void*)hCairoFill,           (void**)&oCairoFill,           "fill" },
    { "cairo.dll", "cairo_pattern_create_rgb",  (void*)hPatCreateRgb,        (void**)&oCairoPatCreateRgb,   "pat-rgb" },
    { "cairo.dll", "cairo_pattern_create_rgba", (void*)hPatCreateRgba,       (void**)&oCairoPatCreateRgba,  "pat-rgba" },
    { "cairo.dll", "cairo_set_source",          (void*)hCairoSetSource,      (void**)&oCairoSetSource,      "set-source" },
    { "cairo.dll", "cairo_set_source_surface",  (void*)hCairoSetSourceSurface,(void**)&oCairoSetSourceSurface,"src-surface" },
    { "cairo.dll", "cairo_destroy",             (void*)hCairoDestroy,        (void**)&oCairoDestroy,         "cr-destroy" },
    { "gdi32.dll", "SetTextColor",              (void*)hSetTextColor,        (void**)&oSetTextColor,        "fg-color" },
    { "gdi32.dll", "SetBkColor",                (void*)hSetBkColor,          (void**)&oSetBkColor,          "bg-color" },
    { "gdi32.dll", "CreateSolidBrush",          (void*)hCreateSolidBrush,    (void**)&oCreateSolidBrush,    "brush" },
    { "gdi32.dll", "ExtTextOutW",               (void*)hExtTextOutW,         (void**)&oExtTextOutW,         "text" },
    { "user32.dll","FillRect",                  (void*)hFillRect,            (void**)&oFillRect,            "bg-fill" },
    { "user32.dll","GetSysColor",               (void*)hGetSysColor,         (void**)&oGetSysColor,         "syscolor" },
    { "user32.dll","GetSysColorBrush",          (void*)hGetSysColorBrush,    (void**)&oGetSysColorBrush,    "sysbrush" },
    { "user32.dll","DrawTextW",                 (void*)hDrawTextW,           (void**)&oDrawTextW,           "drawtext" },
    { "user32.dll","DrawTextExW",               (void*)hDrawTextExW,         (void**)&oDrawTextExW,         "drawtextex" },
};
static void InstallHooks(void)
{
    for (int i = 0; i < (int)(sizeof(g_hooks) / sizeof(g_hooks[0])); i++)
        HookAll(g_hooks[i].dll, g_hooks[i].fn, g_hooks[i].newFn, g_hooks[i].pOld, g_hooks[i].label);
}

// Background re-scan: F1 help windows and other dialogs load their renderer DLLs
// AFTER injection; their IAT would otherwise stay unpatched (light windows).
static volatile long g_stopRescan = 0;
static HANDLE g_hRescan = NULL;
static DWORD WINAPI RescanThread(LPVOID)
{
    while (!g_stopRescan) {
        for (int i = 0; i < 30 && !g_stopRescan; i++) Sleep(100);  // ~3s, responsive stop
        if (g_stopRescan) break;
        InstallHooks();
    }
    return 0;
}

// ---------- Init ----------
static DWORD WINAPI InitThread(LPVOID)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule, path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) sl[1] = 0;
    strcat(path, "themehook_v3_log.txt");
    g_log = fopen(path, "a");
    time_t now = time(NULL);
    Log("=== ThemeHook v3 loaded at %s", ctime(&now));
    Log("[init] process=%d\n", GetCurrentProcessId());

    // pre-cache brushes for safe GetSysColorBrush indices: 5,8,12,13,15,18
    static const int safeIdx[] = { 5, 8, 12, 13, 15, 18 };
    for (int i = 0; i < 6; i++) {
        int idx = safeIdx[i];
        COLORREF orig = GetSysColor(idx);
        g_brCache[idx] = CreateSolidBrush(SysColorEDT(idx, GetSysColor(idx)));
    }

    HMODULE hCairo = GetModuleHandleA("cairo.dll");
    if (hCairo) {
        p_cairo_get_target = (PFN_cairo_get_target)GetProcAddress(hCairo, "cairo_get_target");
        p_cairo_surface_get_dc = (PFN_cairo_win32_surface_get_dc)GetProcAddress(hCairo, "cairo_win32_surface_get_dc");
        p_cairo_img_get_width = (PFN_cairo_image_surface_get_width)GetProcAddress(hCairo, "cairo_image_surface_get_width");
        p_cairo_surf_get_type = (PFN_cairo_surface_get_type)GetProcAddress(hCairo, "cairo_surface_get_type");
        p_cairo_clip_extents = (PFN_cairo_clip_extents)GetProcAddress(hCairo, "cairo_clip_extents");
        p_cairo_get_source = (PFN_cairo_get_source)GetProcAddress(hCairo, "cairo_get_source");
        p_cairo_pattern_get_rgba = (PFN_cairo_pattern_get_rgba)GetProcAddress(hCairo, "cairo_pattern_get_rgba");
        p_img_h = (PFN_cairo_image_surface_get_height)GetProcAddress(hCairo, "cairo_image_surface_get_height");
        p_img_fmt = (PFN_cairo_image_surface_get_format)GetProcAddress(hCairo, "cairo_image_surface_get_format");
        p_img_stride = (PFN_cairo_image_surface_get_stride)GetProcAddress(hCairo, "cairo_image_surface_get_stride");
        p_img_data = (PFN_cairo_image_surface_get_data)GetProcAddress(hCairo, "cairo_image_surface_get_data");
        p_mark_dirty = (PFN_cairo_surface_mark_dirty)GetProcAddress(hCairo, "cairo_surface_mark_dirty");
        p_surf_flush = (PFN_cairo_surface_flush)GetProcAddress(hCairo, "cairo_surface_flush");
        p_img_w = p_cairo_img_get_width;
        Log("[init] cairo_get_target=%p surface_get_dc=%p\n", p_cairo_get_target, p_cairo_surface_get_dc);
    }
    InstallHooks();
    g_hRescan = CreateThread(NULL, 0, RescanThread, NULL, 0, NULL);
    Log("[init] hooking done, %d patch records\n", g_nPatches);
    return 0;
}

static void RestoreIAT(void)
{
    for (int i = 0; i < g_nPatches; i++) {
        DWORD oldProt;
        void** slot = g_patches[i].slot;
        __try {
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                *slot = g_patches[i].orig;
                VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == NULL) {  // FreeLibrary: restore IAT so no dangling pointers remain
            g_stopRescan = 1;
            if (g_hRescan) { WaitForSingleObject(g_hRescan, 2000); CloseHandle(g_hRescan); g_hRescan = NULL; }
            RestoreIAT();
            if (g_log) {
                Log("=== v5.4 unloaded cleanly. CairoRgb=%ld CairoRgba=%ld SetTextColor=%ld SetBkColor=%ld CreateSolidBrush=%ld FillRect=%ld GetSysColor=%ld GetSysColorBrush=%ld ShadowSkip=%ld DupSkip=%ld GdiDup=%ld\n",
                    nCairoRgb, nCairoRgba, nSetTextColor, nSetBkColor, nCreateSolidBrush, nFillRect, nGetSysColor, nGetSysColorBrush, nShadowSkip, nDupSkip, nGdiDup);
                fclose(g_log);
                g_log = NULL;
            }
            DeleteCriticalSection(&g_cs);
        }
    }
    return TRUE;
}
