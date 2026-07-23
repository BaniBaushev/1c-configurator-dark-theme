// PaletteLog.dll - logs all real colors 1C passes to cairo/GDI, with frequency counts.
// Writes palette_dump.txt next to DLL every 5 seconds and on unload. No color substitution.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <time.h>

typedef struct { unsigned rgb; unsigned count; char src; } ColorEntry;
#define MAX_ENTRIES 8192

static HMODULE g_hModule = NULL;
static CRITICAL_SECTION g_cs;
static ColorEntry g_entries[MAX_ENTRIES];
static int g_nEntries = 0;
static volatile LONG g_running = 1;
static HANDLE g_flushThread = NULL;

typedef COLORREF (WINAPI *PFN_SetTextColor)(HDC, COLORREF);
typedef COLORREF (WINAPI *PFN_SetBkColor)(HDC, COLORREF);
typedef HBRUSH (WINAPI *PFN_CreateSolidBrush)(COLORREF);
typedef DWORD (WINAPI *PFN_GetSysColor)(int);
typedef void (CDECL *PFN_cairo_rgb)(void*, double, double, double);
typedef void (CDECL *PFN_cairo_rgba)(void*, double, double, double, double);

static PFN_SetTextColor oSetTextColor = NULL;
static PFN_SetBkColor oSetBkColor = NULL;
static PFN_CreateSolidBrush oCreateSolidBrush = NULL;
static PFN_GetSysColor oGetSysColor = NULL;
static PFN_cairo_rgb oCairoRgb = NULL;
static PFN_cairo_rgba oCairoRgba = NULL;

static void RecordColor(unsigned rgb, char src)
{
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nEntries; i++) {
        if (g_entries[i].rgb == rgb && g_entries[i].src == src) {
            g_entries[i].count++;
            LeaveCriticalSection(&g_cs);
            return;
        }
    }
    if (g_nEntries < MAX_ENTRIES) {
        g_entries[g_nEntries].rgb = rgb;
        g_entries[g_nEntries].count = 1;
        g_entries[g_nEntries].src = src;
        g_nEntries++;
    }
    LeaveCriticalSection(&g_cs);
}

static void Flush(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule, path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) sl[1] = 0;
    strcat(path, "palette_dump.txt");
    FILE* f = fopen(path, "w");
    if (!f) return;
    EnterCriticalSection(&g_cs);
    // simple insertion sort by count desc (top 200)
    fprintf(f, "# palette dump, entries=%d, pid=%d\n", g_nEntries, GetCurrentProcessId());
    fprintf(f, "# src: c=cairo_rgb a=cairo_rgba t=SetTextColor b=SetBkColor s=CreateSolidBrush y=GetSysColor\n");
    for (int i = 0; i < g_nEntries; i++) {
        int best = i;
        for (int j = i + 1; j < g_nEntries; j++)
            if (g_entries[j].count > g_entries[best].count) best = j;
        ColorEntry tmp = g_entries[i]; g_entries[i] = g_entries[best]; g_entries[best] = tmp;
        fprintf(f, "%c #%06X %u\n", g_entries[i].src, g_entries[i].rgb, g_entries[i].count);
    }
    LeaveCriticalSection(&g_cs);
    fclose(f);
}

static DWORD WINAPI FlushThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        Sleep(5000);
        Flush();
    }
    return 0;
}

// ---------- IAT patch bookkeeping for clean unload ----------
#define MAX_PATCHES 4096
typedef struct { void** slot; void* orig; } PatchRec;
static PatchRec g_patches[MAX_PATCHES];
static int g_nPatches = 0;

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

static void HookAll(const char* dll, const char* fn, void* newFn, void** pOld)
{
    int total = 0;
    HMODULE mods[1024]; DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        if (count > 1024) count = 1024;
        for (int i = 0; i < count; i++) total += PatchIAT(mods[i], dll, fn, newFn, pOld);
    }
    if (pOld && !*pOld) {
        HMODULE h = GetModuleHandleA(dll);
        if (h) *pOld = (void*)GetProcAddress(h, fn);
    }
    char path[MAX_PATH];
    GetModuleFileNameA(g_hModule, path, MAX_PATH);
    char* sl = strrchr(path, '\\');
    if (sl) sl[1] = 0;
    strcat(path, "palettelog_init.txt");
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "[hook] %s!%s patched=%d old=%p\n", dll, fn, total, pOld ? *pOld : 0); fclose(f); }
}

// ---------- hooked functions ----------
static unsigned to_rgb(double r, double g, double b)
{
    unsigned R = (unsigned)(r * 255.0 + 0.5); if (R > 255) R = 255;
    unsigned G = (unsigned)(g * 255.0 + 0.5); if (G > 255) G = 255;
    unsigned B = (unsigned)(b * 255.0 + 0.5); if (B > 255) B = 255;
    return (R << 16) | (G << 8) | B;
}

static void CDECL hCairoRgb(void* cr, double r, double g, double b)
{
    RecordColor(to_rgb(r, g, b), 'c');
    oCairoRgb(cr, r, g, b);
}
static void CDECL hCairoRgba(void* cr, double r, double g, double b, double a)
{
    RecordColor(to_rgb(r, g, b), 'a');
    oCairoRgba(cr, r, g, b, a);
}
static COLORREF WINAPI hSetTextColor(HDC hdc, COLORREF c)
{
    RecordColor(c & 0xFFFFFF, 't');
    return oSetTextColor(hdc, c);
}
static COLORREF WINAPI hSetBkColor(HDC hdc, COLORREF c)
{
    RecordColor(c & 0xFFFFFF, 'b');
    return oSetBkColor(hdc, c);
}
static HBRUSH WINAPI hCreateSolidBrush(COLORREF c)
{
    RecordColor(c & 0xFFFFFF, 's');
    return oCreateSolidBrush(c);
}
static DWORD WINAPI hGetSysColor(int idx)
{
    COLORREF c = oGetSysColor(idx);
    RecordColor(c & 0xFFFFFF, 'y');
    return c;
}

static DWORD WINAPI InitThread(LPVOID)
{
    InitializeCriticalSection(&g_cs);
    HookAll("cairo.dll", "cairo_set_source_rgb",  (void*)hCairoRgb,  (void**)&oCairoRgb);
    HookAll("cairo.dll", "cairo_set_source_rgba", (void*)hCairoRgba, (void**)&oCairoRgba);
    HookAll("gdi32.dll", "SetTextColor",    (void*)hSetTextColor,    (void**)&oSetTextColor);
    HookAll("gdi32.dll", "SetBkColor",      (void*)hSetBkColor,      (void**)&oSetBkColor);
    HookAll("gdi32.dll", "CreateSolidBrush",(void*)hCreateSolidBrush,(void**)&oCreateSolidBrush);
    HookAll("user32.dll","GetSysColor",     (void*)hGetSysColor,     (void**)&oGetSysColor);
    g_flushThread = CreateThread(NULL, 0, FlushThread, NULL, 0, NULL);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
        HANDLE h = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_running, 0);
        if (reserved == NULL) {           // FreeLibrary, not process exit
            for (int i = 0; i < g_nPatches; i++) {
                DWORD oldProt;
                __try {
                    if (VirtualProtect(g_patches[i].slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                        *g_patches[i].slot = g_patches[i].orig;
                        VirtualProtect(g_patches[i].slot, sizeof(void*), oldProt, &oldProt);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            if (g_flushThread) {
                WaitForSingleObject(g_flushThread, 6000);
                CloseHandle(g_flushThread);
            }
            Flush();
            DeleteCriticalSection(&g_cs);
        }
    }
    return TRUE;
}
