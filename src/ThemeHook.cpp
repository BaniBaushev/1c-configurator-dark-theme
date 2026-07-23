// ThemeHook.dll - PoC #2: proxy-GDI dark theme for 1cv8.exe via IAT hooks
// Build (x86): cl /LD /MD ThemeHook.cpp /link user32.lib gdi32.lib
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <time.h>

// ---------- Tunables ----------
static const int  LIGHT_SUM_THRESHOLD = 600;  // R+G+B > 600 => "light" color
static const COLORREF DARK_BG   = RGB(0x1E, 0x1E, 0x1E);
static const COLORREF LIGHT_FG  = RGB(0xD4, 0xD4, 0xD4);
static const int  LOG_FIRST_N   = 30;   // log first N hits per function
// ------------------------------

static HMODULE g_hModule = NULL;
static CRITICAL_SECTION g_cs;
static FILE* g_log = NULL;

// Originals
typedef int (WINAPI *PFN_FillRect)(HDC, const RECT*, HBRUSH);
typedef COLORREF (WINAPI *PFN_SetBkColor)(HDC, COLORREF);
typedef COLORREF (WINAPI *PFN_SetTextColor)(HDC, COLORREF);
typedef BOOL (WINAPI *PFN_ExtTextOutW)(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
typedef HBRUSH (WINAPI *PFN_CreateSolidBrush)(COLORREF);
typedef void (CDECL *PFN_cairo_set_source_rgb)(void*, double, double, double);
typedef void (CDECL *PFN_cairo_set_source_rgba)(void*, double, double, double, double);
typedef DWORD (WINAPI *PFN_GetSysColor)(int);
typedef HBRUSH (WINAPI *PFN_GetSysColorBrush)(int);

static PFN_FillRect        oFillRect        = NULL;
static PFN_SetBkColor      oSetBkColor      = NULL;
static PFN_SetTextColor    oSetTextColor    = NULL;
static PFN_ExtTextOutW     oExtTextOutW     = NULL;
static PFN_CreateSolidBrush oCreateSolidBrush = NULL;
static PFN_cairo_set_source_rgb  oCairoRgb  = NULL;
static PFN_cairo_set_source_rgba oCairoRgba = NULL;
static PFN_GetSysColor     oGetSysColor     = NULL;
static PFN_GetSysColorBrush oGetSysColorBrush = NULL;

static long nFillRect = 0, nSetBkColor = 0, nSetTextColor = 0, nExtTextOut = 0, nCreateSolidBrush = 0;
static long nCairoRgb = 0, nCairoRgba = 0, nGetSysColor = 0, nGetSysColorBrush = 0;
static HBRUSH g_darkBrush = NULL, g_panelBrush = NULL, g_fgBrush = NULL;

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

static bool IsLight(COLORREF c)
{
    return (int)GetRValue(c) + GetGValue(c) + GetBValue(c) > LIGHT_SUM_THRESHOLD;
}
static bool IsDark(COLORREF c)
{
    return (int)GetRValue(c) + GetGValue(c) + GetBValue(c) < 200;
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
                DWORD oldProt;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    if (pOld && !*pOld) *pOld = (void*)thunk->u1.Function;
                    thunk->u1.Function = (DWORD_PTR)newFn;
                    VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                    patched++;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return patched;
}

static void HookAllModules(const char* dllName, const char* fnName, void* newFn, void** pOld, const char* label)
{
    int total = 0;
    HMODULE mods[1024]; DWORD needed = 0;
    HANDLE hProc = GetCurrentProcess();
    if (EnumProcessModules(hProc, mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        if (count > 1024) count = 1024;
        for (int i = 0; i < count; i++)
            total += PatchIAT(mods[i], dllName, fnName, newFn, pOld);
    }
    Log("[hook] %s!%s (%s): patched %d IAT entries\n", dllName, fnName, label, total);
    // Fallback: if no IAT entry existed anywhere, get original from the system DLL
    if (pOld && !*pOld) {
        HMODULE h = GetModuleHandleA(dllName);
        if (h) *pOld = (void*)GetProcAddress(h, fnName);
    }
}

// ---------- Hooked functions ----------
static int WINAPI hFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
{
    InterlockedIncrement(&nFillRect);
    if ((DWORD_PTR)hbr < 0x10000) {
        // system color brush like (HBRUSH)(COLOR_WINDOW+1)
        int idx = (int)(DWORD_PTR)hbr - 1;
        COLORREF c = oGetSysColor ? oGetSysColor(idx) : GetSysColor(idx);
        if (IsLight(c)) hbr = GetSysColorBrush(COLOR_3DFACE); // temp marker; replaced below
        if (IsLight(c)) {
            if (nFillRect <= LOG_FIRST_N)
                Log("[FillRect #%ld] syscolor idx=%d %06X -> DARK_BG\n", nFillRect, idx, c);
            HBRUSH dark = CreateSolidBrush(DARK_BG);
            int r = oFillRect(hdc, lprc, dark);
            DeleteObject(dark);
            return r;
        }
    } else {
        // Try to peek brush color via GetObject on LOGBRUSH (works for solid brushes)
        LOGBRUSH lb;
        if (GetObjectW(hbr, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID && IsLight(lb.lbColor)) {
            if (nFillRect <= LOG_FIRST_N)
                Log("[FillRect #%ld] solid %06X -> DARK_BG\n", nFillRect, lb.lbColor);
            HBRUSH dark = CreateSolidBrush(DARK_BG);
            int r = oFillRect(hdc, lprc, dark);
            DeleteObject(dark);
            return r;
        }
    }
    return oFillRect(hdc, lprc, hbr);
}

static COLORREF WINAPI hSetBkColor(HDC hdc, COLORREF c)
{
    InterlockedIncrement(&nSetBkColor);
    if (IsLight(c)) {
        if (nSetBkColor <= LOG_FIRST_N)
            Log("[SetBkColor #%ld] %06X -> DARK_BG\n", nSetBkColor, c);
        return oSetBkColor(hdc, DARK_BG);
    }
    return oSetBkColor(hdc, c);
}

static COLORREF WINAPI hSetTextColor(HDC hdc, COLORREF c)
{
    InterlockedIncrement(&nSetTextColor);
    if (IsDark(c)) {
        if (nSetTextColor <= LOG_FIRST_N)
            Log("[SetTextColor #%ld] %06X -> LIGHT_FG\n", nSetTextColor, c);
        return oSetTextColor(hdc, LIGHT_FG);
    }
    return oSetTextColor(hdc, c);
}

static HBRUSH WINAPI hCreateSolidBrush(COLORREF c)
{
    InterlockedIncrement(&nCreateSolidBrush);
    if (IsLight(c)) {
        if (nCreateSolidBrush <= LOG_FIRST_N)
            Log("[CreateSolidBrush #%ld] %06X -> DARK_BG\n", nCreateSolidBrush, c);
        return oCreateSolidBrush(DARK_BG);
    }
    return oCreateSolidBrush(c);
}

static BOOL WINAPI hExtTextOutW(HDC hdc, int x, int y, UINT opt, const RECT* rc, LPCWSTR str, UINT len, const INT* dx)
{
    InterlockedIncrement(&nExtTextOut);
    if (nExtTextOut <= LOG_FIRST_N)
        Log("[ExtTextOutW #%ld] opt=%u len=%u\n", nExtTextOut, opt, len);
    return oExtTextOutW(hdc, x, y, opt, rc, str, len, dx);
}

// ---------- Cairo color hooks (grphcs.dll -> cairo.dll) ----------
static const double DARK_BG_F  = 0x1E / 255.0;   // ~0.118
static const double PANEL_F    = 0x2D / 255.0;   // ~0.176
static const double LIGHT_FG_F = 0xD4 / 255.0;   // ~0.831

static void CDECL hCairoSetSourceRgb(void* cr, double r, double g, double b)
{
    InterlockedIncrement(&nCairoRgb);
    double sum = r + g + b;
    if (sum > 2.0) {           // light fill -> dark background
        if (nCairoRgb <= LOG_FIRST_N)
            Log("[cairo_rgb #%ld] %.2f %.2f %.2f -> DARK\n", nCairoRgb, r, g, b);
        r = g = b = DARK_BG_F;
    } else if (sum < 0.67) {   // dark fill (text) -> light foreground
        if (nCairoRgb <= LOG_FIRST_N)
            Log("[cairo_rgb #%ld] %.2f %.2f %.2f -> LIGHT\n", nCairoRgb, r, g, b);
        r = g = b = LIGHT_FG_F;
    }
    oCairoRgb(cr, r, g, b);
}

static void CDECL hCairoSetSourceRgba(void* cr, double r, double g, double b, double a)
{
    InterlockedIncrement(&nCairoRgba);
    double sum = r + g + b;
    if (sum > 2.0) {
        if (nCairoRgba <= LOG_FIRST_N)
            Log("[cairo_rgba #%ld] %.2f %.2f %.2f a=%.2f -> DARK\n", nCairoRgba, r, g, b, a);
        r = g = b = DARK_BG_F;
    } else if (sum < 0.67) {
        if (nCairoRgba <= LOG_FIRST_N)
            Log("[cairo_rgba #%ld] %.2f %.2f %.2f a=%.2f -> LIGHT\n", nCairoRgba, r, g, b, a);
        r = g = b = LIGHT_FG_F;
    }
    oCairoRgba(cr, r, g, b, a);
}

// ---------- GetSysColor hooks ----------
static COLORREF MapSysColor(int idx, COLORREF c)
{
    switch (idx) {
    case 5:  // COLOR_WINDOW
    case 12: // COLOR_APPWORKSPACE
        return DARK_BG;
    case 15: // COLOR_BTNFACE / 3DFACE
    case 16: // COLOR_BTNSHADOW-ish, keep dark-ish
        return RGB(0x2D, 0x2D, 0x2D);
    case 8:  // COLOR_WINDOWTEXT
    case 18: // COLOR_BTNTEXT
        return LIGHT_FG;
    default:
        return IsLight(c) ? DARK_BG : c;
    }
}

static DWORD WINAPI hGetSysColor(int idx)
{
    InterlockedIncrement(&nGetSysColor);
    COLORREF c = oGetSysColor(idx);
    COLORREF m = MapSysColor(idx, c);
    if (m != c && nGetSysColor <= LOG_FIRST_N)
        Log("[GetSysColor #%ld] idx=%d %06X -> %06X\n", nGetSysColor, idx, c, m);
    return m;
}

static HBRUSH WINAPI hGetSysColorBrush(int idx)
{
    InterlockedIncrement(&nGetSysColorBrush);
    if (nGetSysColorBrush <= LOG_FIRST_N)
        Log("[GetSysColorBrush #%ld] idx=%d -> dark brush\n", nGetSysColorBrush, idx);
    switch (idx) {
    case 5: case 12: return g_darkBrush;
    case 15: case 16: return g_panelBrush;
    case 8: case 18: return g_fgBrush;
    default: return g_panelBrush;
    }
}

// ---------- Init thread (outside DllMain loader lock) ----------
static DWORD WINAPI InitThread(LPVOID)
{
    // Log to folder next to the DLL
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule, path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) sl[1] = 0;
    strcat(path, "themehook_log.txt");
    g_log = fopen(path, "a");
    if (!g_log) {
        GetTempPathA(MAX_PATH, path);
        strcat(path, "themehook_log.txt");
        g_log = fopen(path, "a");
    }
    time_t now = time(NULL);
    Log("=== ThemeHook loaded (x86) at %s", ctime(&now));
    Log("[init] process=%d\n", GetCurrentProcessId());

    g_darkBrush  = CreateSolidBrush(DARK_BG);
    g_panelBrush = CreateSolidBrush(RGB(0x2D, 0x2D, 0x2D));
    g_fgBrush    = CreateSolidBrush(LIGHT_FG);

    HookAllModules("user32.dll", "FillRect",         (void*)hFillRect,        (void**)&oFillRect,        "bg-fill");
    HookAllModules("gdi32.dll",  "SetBkColor",       (void*)hSetBkColor,      (void**)&oSetBkColor,      "bg-color");
    HookAllModules("gdi32.dll",  "SetTextColor",     (void*)hSetTextColor,    (void**)&oSetTextColor,    "fg-color");
    HookAllModules("gdi32.dll",  "CreateSolidBrush", (void*)hCreateSolidBrush,(void**)&oCreateSolidBrush,"brush");
    HookAllModules("gdi32.dll",  "ExtTextOutW",      (void*)hExtTextOutW,     (void**)&oExtTextOutW,     "text");
    HookAllModules("cairo.dll",  "cairo_set_source_rgb",  (void*)hCairoSetSourceRgb,  (void**)&oCairoRgb,  "cairo-fill");
    HookAllModules("cairo.dll",  "cairo_set_source_rgba", (void*)hCairoSetSourceRgba, (void**)&oCairoRgba,"cairo-filla");
    HookAllModules("user32.dll", "GetSysColor",      (void*)hGetSysColor,     (void**)&oGetSysColor,     "syscolor");
    HookAllModules("user32.dll", "GetSysColorBrush", (void*)hGetSysColorBrush,(void**)&oGetSysColorBrush,"sysbrush");
    Log("[init] hooking done\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) {
            Log("=== ThemeHook unloaded. FillRect=%ld SetBkColor=%ld SetTextColor=%ld CreateSolidBrush=%ld ExtTextOutW=%ld CairoRgb=%ld CairoRgba=%ld GetSysColor=%ld GetSysColorBrush=%ld\n",
                nFillRect, nSetBkColor, nSetTextColor, nCreateSolidBrush, nExtTextOut, nCairoRgb, nCairoRgba, nGetSysColor, nGetSysColorBrush);
            fclose(g_log);
            g_log = NULL;
        }
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
