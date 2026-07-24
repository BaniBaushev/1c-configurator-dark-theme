// IconDump.dll - PoC: дампит иконки, которыми 1С рисует UI через Cairo.
// Хук: cairo_set_source_surface (EAT-патч cairo.dll, как в ThemeHook3).
// Для image-surface размером 8..64 px считается хэш пикселей; новые иконки
// сохраняются в icons_dump/<w>x<h>_<hash>.png + журнал icons_dump/index.txt.
// Безопасность: только чтение surface, никакой записи в контексты (SKIP-ONLY).
// Выгрузка: откат EAT при DETACH.
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <psapi.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

// ---------- cairo types ----------
typedef void cairo_t;
typedef void cairo_surface_t;
typedef int cairo_format_t;

typedef void (*cairo_set_source_surface_t)(cairo_t*, cairo_surface_t*, double, double);
typedef void* cairo_pattern_t;
typedef cairo_pattern_t* cairo_pattern_t_ptr;
typedef cairo_pattern_t_ptr (*cairo_pattern_create_for_surface_t)(cairo_surface_t*);
typedef void (*cairo_mask_surface_t)(cairo_t*, cairo_surface_t*, double, double);
typedef cairo_pattern_t_ptr (*cairo_get_source_t)(cairo_t*);
typedef int (*cairo_pattern_get_surface_t)(cairo_pattern_t_ptr, cairo_surface_t**);
typedef cairo_surface_t* (*cairo_get_target_t)(cairo_t*);
typedef int  (*cairo_surface_get_type_t)(cairo_surface_t*);
typedef int  (*cairo_image_surface_get_width_t)(cairo_surface_t*);
typedef int  (*cairo_image_surface_get_height_t)(cairo_surface_t*);
typedef cairo_format_t (*cairo_image_surface_get_format_t)(cairo_surface_t*);
typedef int  (*cairo_image_surface_get_stride_t)(cairo_surface_t*);
typedef unsigned char* (*cairo_image_surface_get_data_t)(cairo_surface_t*);
typedef int (*cairo_surface_write_to_png_t)(cairo_surface_t*, const char*);
typedef void (*cairo_surface_flush_t)(cairo_surface_t*);

static cairo_set_source_surface_t      orig_set_source_surface = NULL;
static cairo_pattern_create_for_surface_t orig_pattern_create_for_surface = NULL;
static cairo_mask_surface_t            orig_mask_surface = NULL;
static cairo_image_surface_get_width_t  p_get_width  = NULL;
static cairo_image_surface_get_height_t p_get_height = NULL;
static cairo_image_surface_get_format_t p_get_format = NULL;
static cairo_image_surface_get_stride_t p_get_stride = NULL;
static cairo_image_surface_get_data_t   p_get_data   = NULL;
static cairo_surface_write_to_png_t     p_write_png  = NULL;
static cairo_surface_flush_t            p_flush      = NULL;

#define CAIRO_SURFACE_TYPE_IMAGE 0
#define CAIRO_FORMAT_ARGB32 0
#define CAIRO_FORMAT_RGB24  1

// ---------- глобальное состояние ----------
static HMODULE g_hModule = NULL;
static FILE* g_log = NULL;
static CRITICAL_SECTION g_cs;
static char g_dumpDir[MAX_PATH] = {0};
static long g_seen = 0, g_saved = 0;

// журнал хэшей (простой дедуп, 64К записей)
#define HASHLOG_CAP 65536
static unsigned long long g_hashes[HASHLOG_CAP];
static int g_hashCount = 0;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// FNV-1a по пикселям surface (только чтение)
static unsigned long long HashSurface(cairo_surface_t* s, int w, int h, int fmt)
{
    int stride = p_get_stride(s);
    unsigned char* data = p_get_data(s);
    if (!data || stride <= 0) return 0;
    unsigned long long hash = 1469598103934665603ULL;
    int bpp = 4;
    for (int y = 0; y < h; y++) {
        unsigned char* row = data + (long long)y * stride;
        for (int x = 0; x < w * bpp; x++) {
            // альфа-канал нормализуем: полностью прозрачные пиксели у разных
            // иконок могут иметь мусор в RGB
            unsigned char v = row[x];
            if (fmt == CAIRO_FORMAT_ARGB32 && (x & 3) != 3) {
                unsigned char a = row[(x & ~3) | 3];
                if (a == 0) v = 0;
            }
            hash ^= v;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

static int HashSeen(unsigned long long hh)
{
    for (int i = 0; i < g_hashCount; i++)
        if (g_hashes[i] == hh) return 1;
    if (g_hashCount < HASHLOG_CAP) g_hashes[g_hashCount++] = hh;
    return 0;
}

// ---------- hook ----------
static long g_logCapSS = 200, g_logCapPat = 200, g_logCapMask = 200;

static void TryDump(cairo_surface_t* surface, const char* via)
{
    if (!surface || !p_get_width) return;
    int w = p_get_width(surface);
    int h = p_get_height(surface);
    long* cap = (via[0]=='s') ? &g_logCapSS : (via[0]=='p') ? &g_logCapPat : &g_logCapMask;
    if (*cap > 0) {
        EnterCriticalSection(&g_cs);
        if (*cap > 0) { (*cap)--; Log("[trace] %s surface=%p %dx%d fmt=%d\n", via, surface, w, h, p_get_format(surface)); }
        LeaveCriticalSection(&g_cs);
    }
    if (w < 8 || w > 64 || h < 8 || h > 64) return;
    int fmt = p_get_format(surface);
    if (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24) return;
    unsigned long long hh = HashSurface(surface, w, h, fmt);
    if (!hh) return;
    EnterCriticalSection(&g_cs);
    InterlockedIncrement(&g_seen);
    if (!HashSeen(hh)) {
        char path[MAX_PATH];
        sprintf(path, "%s\\%02dx%02d_%016llX.png", g_dumpDir, w, h, hh);
        p_flush(surface);
        int st = p_write_png(surface, path);
        InterlockedIncrement(&g_saved);
        Log("[%5ld] %dx%d fmt=%d hash=%016llX via=%s write=%d\n",
            g_saved, w, h, fmt, hh, via, st);
    }
    LeaveCriticalSection(&g_cs);
}

static void hooked_set_source_surface(cairo_t* cr, cairo_surface_t* surface, double x, double y)
{
    TryDump(surface, "set_source_surface");
    orig_set_source_surface(cr, surface, x, y);
}

static cairo_pattern_t_ptr hooked_pattern_create_for_surface(cairo_surface_t* surface)
{
    TryDump(surface, "pattern_create_for_surface");
    return orig_pattern_create_for_surface(surface);
}

static void hooked_mask_surface(cairo_t* cr, cairo_surface_t* surface, double x, double y)
{
    TryDump(surface, "mask_surface");
    orig_mask_surface(cr, surface, x, y);
}

// ---------- GDI: ImageList ----------
typedef struct _IMAGELISTDRAWPARAMS {
    DWORD cbSize; void* himl; int i; HDC hdcDst; int x; int y; int cx; int cy;
    int xBitmap; int yBitmap; COLORREF rgbBk; COLORREF rgbFg; UINT fStyle;
    DWORD dwRop; DWORD fState; DWORD Frame; COLORREF crEffect;
} IMAGELISTDRAWPARAMS, *LPIMAGELISTDRAWPARAMS;

typedef BOOL (WINAPI *ImageList_Draw_t)(void* himl, int i, HDC hdc, int x, int y, UINT style);
typedef BOOL (WINAPI *ImageList_DrawIndirect_t)(LPIMAGELISTDRAWPARAMS p);
typedef int  (WINAPI *ImageList_GetImageCount_t)(void* himl);
typedef HICON (WINAPI *ImageList_GetIcon_t)(void* himl, int i, UINT flags);

static ImageList_Draw_t          orig_IL_Draw = NULL;
static ImageList_DrawIndirect_t  orig_IL_DrawIndirect = NULL;
static ImageList_GetImageCount_t p_IL_GetImageCount = NULL;
static ImageList_GetIcon_t       p_IL_GetIcon = NULL;

#define MAX_IL_DUMPED 256
static void* g_ilDumped[MAX_IL_DUMPED];
static int g_ilDumpedCount = 0;

static void DumpBitmapBits(HBITMAP hbm, int w, int h, const char* tag, int idx)
{
    BITMAPINFOHEADER bih = {0};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h;  // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    HDC hdc = GetDC(NULL);
    int sz = w * h * 4;
    unsigned char* buf = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, sz);
    if (!buf) { ReleaseDC(NULL, hdc); return; }
    if (GetDIBits(hdc, hbm, 0, h, buf, (BITMAPINFO*)&bih, DIB_RGB_COLORS)) {
        char path[MAX_PATH];
        sprintf(path, "%s\\gdi_%s_%03d_%02dx%02d.bmp", g_dumpDir, tag, idx, w, h);
        FILE* f = fopen(path, "wb");
        if (f) {
            BITMAPFILEHEADER bfh = {'B' | ('M'<<8), (DWORD)(54 + sz), 0, 0, 54};
            BITMAPINFOHEADER out = bih; out.biHeight = h;
            fwrite(&bfh, 14, 1, f);
            fwrite(&out, 40, 1, f);
            fwrite(buf, sz, 1, f);
            fclose(f);
            InterlockedIncrement(&g_saved);
            Log("[%5ld] GDI %s icon %d: %dx%d -> %s\n", g_saved, tag, idx, w, h, path);
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    ReleaseDC(NULL, hdc);
}

static void TryDumpImageList(void* himl)
{
    if (!himl || !p_IL_GetImageCount || !p_IL_GetIcon) return;
    EnterCriticalSection(&g_cs);
    int seen = 0;
    for (int i = 0; i < g_ilDumpedCount; i++) if (g_ilDumped[i] == himl) { seen = 1; break; }
    if (!seen && g_ilDumpedCount < MAX_IL_DUMPED) g_ilDumped[g_ilDumpedCount++] = himl;
    LeaveCriticalSection(&g_cs);
    if (seen) return;
    int n = p_IL_GetImageCount(himl);
    Log("[il] himl=%p count=%d\n", himl, n);
    if (n > 200) n = 200;
    for (int i = 0; i < n; i++) {
        HICON hi = p_IL_GetIcon(himl, i, 0);
        if (!hi) continue;
        ICONINFO ii;
        if (GetIconInfo(hi, &ii)) {
            BITMAP bm;
            if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm))
                DumpBitmapBits(ii.hbmColor, bm.bmWidth, bm.bmHeight, "il", i);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
        }
        DestroyIcon(hi);
    }
}

static BOOL WINAPI hooked_ImageList_Draw(void* himl, int i, HDC hdc, int x, int y, UINT style)
{
    TryDumpImageList(himl);
    return orig_IL_Draw(himl, i, hdc, x, y, style);
}

static BOOL WINAPI hooked_ImageList_DrawIndirect(LPIMAGELISTDRAWPARAMS p)
{
    if (p) TryDumpImageList(p->himl);
    return orig_IL_DrawIndirect(p);
}

// ---------- IAT patch (обязателен: уже привязанные модули зовут оригиналы через свою IAT) ----------
struct IatTarget { const char* func; void* hookFn; void** origSlot; };
static void** g_orig_ss = (void**)&orig_set_source_surface;
static void** g_orig_pcs = (void**)&orig_pattern_create_for_surface;
static void** g_orig_mask = (void**)&orig_mask_surface;
static void** g_orig_ild = (void**)&orig_IL_Draw;
static void** g_orig_ildi = (void**)&orig_IL_DrawIndirect;

struct IatRec { void** slot; void* orig; };
static IatRec g_iat[512];
static int g_iatCount = 0;

static void IatPatchModule(HMODULE hMod, const char* func, void* origFn, void* hookFn)
{
    if (!hMod || hMod == g_hModule) return;
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA* tn = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* tf = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; tn->u1.AddressOfData; tn++, tf++) {
            if (tn->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + tn->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) != 0) continue;
            void** slot = (void**)&tf->u1.Function;
            if (*slot != origFn) continue;  // уже пропатчен (нашим же EAT не трогаем: origFn — оригинал)
            DWORD old;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                if (g_iatCount < 512) { g_iat[g_iatCount].slot = slot; g_iat[g_iatCount].orig = origFn; g_iatCount++; }
                *slot = hookFn;
                VirtualProtect(slot, sizeof(void*), old, &old);
            }
        }
    }
}

static void IatPatchAll(const char* func, void* origFn, void* hookFn, const char* tag)
{
    HMODULE mods[256];
    DWORD need = 0;
    int n = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        n = need / sizeof(HMODULE);
        if (n > 256) n = 256;
    }
    int before = g_iatCount;
    for (int i = 0; i < n; i++) IatPatchModule(mods[i], func, origFn, hookFn);
    Log("[ok] IAT %s: patched %d slots in %d modules\n", tag, g_iatCount - before, n);
}

static void IatRestore(void)
{
    for (int i = 0; i < g_iatCount; i++) {
        DWORD old;
        if (VirtualProtect(g_iat[i].slot, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_iat[i].slot = g_iat[i].orig;
            VirtualProtect(g_iat[i].slot, sizeof(void*), old, &old);
        }
    }
    g_iatCount = 0;
}

// ---------- EAT patch ----------
struct PatchRec { void* addr; void* orig; HMODULE hMod; };
static PatchRec g_patches[8];
static int g_patchCount = 0;

static void* EatPatch(HMODULE hMod, const char* func, void* hookFn)
{
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return NULL;
    IMAGE_EXPORT_DIRECTORY* exp = (IMAGE_EXPORT_DIRECTORY*)(base + dd.VirtualAddress);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
    WORD*  ords  = (WORD*)(base + exp->AddressOfNameOrdinals);
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* nm = (const char*)(base + names[i]);
        if (strcmp(nm, func) == 0) {
            DWORD* slot = &funcs[ords[i]];
            DWORD rvaHook = (DWORD)((BYTE*)hookFn - base);
            DWORD old;
            if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old)) return NULL;
            void* origFn = base + *slot;
            *slot = rvaHook;
            VirtualProtect(slot, sizeof(DWORD), old, &old);
            if (g_patchCount < 8) {
                g_patches[g_patchCount].addr = slot;
                g_patches[g_patchCount].orig = origFn;
                g_patches[g_patchCount].hMod = hMod;
                g_patchCount++;
            }
            return origFn;
        }
    }
    return NULL;
}

static void EatRestore(void)
{
    for (int i = 0; i < g_patchCount; i++) {
        HMODULE hm = GetModuleHandleA(NULL); // placeholder
        hm = g_patches[i].hMod;
        if (!hm) continue;
        DWORD rva = (DWORD)((BYTE*)g_patches[i].orig - (BYTE*)hm);
        DWORD old;
        if (VirtualProtect(g_patches[i].addr, sizeof(DWORD), PAGE_READWRITE, &old)) {
            *(DWORD*)g_patches[i].addr = rva;
            VirtualProtect(g_patches[i].addr, sizeof(DWORD), old, &old);
        }
        g_patches[i].addr = NULL;
    }
    g_patchCount = 0;
}

// ---------- init ----------
static DWORD WINAPI InitThread(LPVOID)
{
    GetTempPathA(MAX_PATH, g_dumpDir);
    strcat(g_dumpDir, "icons_dump");
    CreateDirectoryA(g_dumpDir, NULL);

    char logpath[MAX_PATH];
    strcpy(logpath, g_dumpDir);
    strcat(logpath, "\\index.txt");
    g_log = fopen(logpath, "a");
    time_t now = time(NULL);
    Log("=== IconDump loaded at %s", ctime(&now));

    HMODULE hcairo = GetModuleHandleA("cairo.dll");
    if (!hcairo) { Log("[!] cairo.dll not loaded\n"); return 0; }

    p_get_width  = (cairo_image_surface_get_width_t) GetProcAddress(hcairo, "cairo_image_surface_get_width");
    p_get_height = (cairo_image_surface_get_height_t)GetProcAddress(hcairo, "cairo_image_surface_get_height");
    p_get_format = (cairo_image_surface_get_format_t)GetProcAddress(hcairo, "cairo_image_surface_get_format");
    p_get_stride = (cairo_image_surface_get_stride_t)GetProcAddress(hcairo, "cairo_image_surface_get_stride");
    p_get_data   = (cairo_image_surface_get_data_t)  GetProcAddress(hcairo, "cairo_image_surface_get_data");
    p_write_png  = (cairo_surface_write_to_png_t)    GetProcAddress(hcairo, "cairo_surface_write_to_png");
    p_flush      = (cairo_surface_flush_t)           GetProcAddress(hcairo, "cairo_surface_flush");
    if (!p_get_width || !p_get_data || !p_write_png) {
        Log("[!] missing cairo exports\n"); return 0;
    }

    void* orig = EatPatch(hcairo, "cairo_set_source_surface", (void*)hooked_set_source_surface);
    if (!orig) { Log("[!] EAT patch set_source_surface failed\n"); return 0; }
    orig_set_source_surface = (cairo_set_source_surface_t)orig;
    Log("[ok] cairo_set_source_surface hooked (EAT)\n");

    orig = EatPatch(hcairo, "cairo_pattern_create_for_surface", (void*)hooked_pattern_create_for_surface);
    if (orig) {
        orig_pattern_create_for_surface = (cairo_pattern_create_for_surface_t)orig;
        Log("[ok] cairo_pattern_create_for_surface hooked (EAT)\n");
    } else Log("[!] EAT patch pattern_create_for_surface failed\n");

    orig = EatPatch(hcairo, "cairo_mask_surface", (void*)hooked_mask_surface);
    if (orig) {
        orig_mask_surface = (cairo_mask_surface_t)orig;
        Log("[ok] cairo_mask_surface hooked (EAT)\n");
    } else Log("[!] EAT patch mask_surface failed\n");

    // GDI ImageList (дерево конфигурации и пр.)
    HMODULE hcomctl = GetModuleHandleA("comctl32.dll");
    if (!hcomctl) hcomctl = LoadLibraryA("comctl32.dll");
    if (hcomctl) {
        p_IL_GetImageCount = (ImageList_GetImageCount_t)GetProcAddress(hcomctl, "ImageList_GetImageCount");
        p_IL_GetIcon       = (ImageList_GetIcon_t)      GetProcAddress(hcomctl, "ImageList_GetIcon");
        orig = EatPatch(hcomctl, "ImageList_Draw", (void*)hooked_ImageList_Draw);
        if (orig) { orig_IL_Draw = (ImageList_Draw_t)orig; Log("[ok] comctl32!ImageList_Draw hooked (EAT)\n"); }
        else Log("[!] EAT patch ImageList_Draw failed\n");
        orig = EatPatch(hcomctl, "ImageList_DrawIndirect", (void*)hooked_ImageList_DrawIndirect);
        if (orig) { orig_IL_DrawIndirect = (ImageList_DrawIndirect_t)orig; Log("[ok] comctl32!ImageList_DrawIndirect hooked (EAT)\n"); }
        else Log("[!] EAT patch ImageList_DrawIndirect failed\n");
    } else Log("[!] comctl32.dll not found\n");

    // IAT-патчи: уже загруженные модули зовут оригиналы через свою IAT
    if (orig_set_source_surface)
        IatPatchAll("cairo_set_source_surface", (void*)orig_set_source_surface, (void*)hooked_set_source_surface, "set_source_surface");
    if (orig_pattern_create_for_surface)
        IatPatchAll("cairo_pattern_create_for_surface", (void*)orig_pattern_create_for_surface, (void*)hooked_pattern_create_for_surface, "pattern_create_for_surface");
    if (orig_mask_surface)
        IatPatchAll("cairo_mask_surface", (void*)orig_mask_surface, (void*)hooked_mask_surface, "mask_surface");
    if (orig_IL_Draw)
        IatPatchAll("ImageList_Draw", (void*)orig_IL_Draw, (void*)hooked_ImageList_Draw, "ImageList_Draw");
    if (orig_IL_DrawIndirect)
        IatPatchAll("ImageList_DrawIndirect", (void*)orig_IL_DrawIndirect, (void*)hooked_ImageList_DrawIndirect, "ImageList_DrawIndirect");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = (HMODULE)h;
        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls((HMODULE)h);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        IatRestore();
        EatRestore();
        if (g_log) {
            Log("=== IconDump unloaded. surfaces_seen=%ld saved=%ld\n", g_seen, g_saved);
            fclose(g_log);
            g_log = NULL;
        }
        DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
