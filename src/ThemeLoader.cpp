// ThemeLoader.dll - минимальный verifier-provider для IFEO-автозагрузки темы 1С.
// Зачем отдельный загрузчик:
//   Прямая регистрация ThemeHook3_v55.dll в IFEO (VerifierDlls) дала краш
//   0xc0000142 (STATUS_DLL_INIT_FAILED) при старте 1cv8.exe: DLL с динамическим
//   CRT (/MD) и тяжёлым DllMain (CreateThread, fopen) загружается verifier'ом
//   в слишком раннем контексте инициализации процесса.
// Решение:
//   Этот загрузчик собран со статическим CRT (/MT) и НЕ использует CRT вообще.
//   DllMain пустой. Реальная загрузка темы - в VerifierDllInitialize, которую
//   ntdll вызывает ПОСЛЕ DllMain всех verifier-провайдеров, вне loader lock.
// Экспорты (через ThemeLoader.def, без декорирования x86 stdcall):
//   VerifierDllInitialize / VerifierDllUninitialize
#include <windows.h>

// Путь к DLL темы вычисляется относительно расположения самого ThemeLoader.dll:
// тема ищется рядом с загрузчиком (обе лежат в builds/). Работает у любого
// пользователя после клонирования репозитория, без правки исходника.
static HMODULE g_self = NULL;

static void GetThemeDllPath(WCHAR* out, DWORD cap)
{
    DWORD n = GetModuleFileNameW(g_self, out, cap);
    if (n == 0 || n >= cap) { out[0] = 0; return; }
    WCHAR* sl = out;
    for (WCHAR* p = out; *p; ++p) if (*p == L'\\') sl = p;
    lstrcpynW(sl + 1, L"ThemeHook3_v55.dll", (DWORD)(cap - (sl + 1 - out)));
}

extern "C" __declspec(dllexport) BOOL WINAPI VerifierDllInitialize(DWORD dwReason)
{
    (void)dwReason;
    WCHAR path[MAX_PATH];
    GetThemeDllPath(path, MAX_PATH);
    if (path[0]) {
        HMODULE h = LoadLibraryW(path);
        // TRUE в любом случае: даже если тема не найдена/не загрузилась,
        // конфигуратор обязан запуститься (тема - опция, не блокер).
        (void)h;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) VOID WINAPI VerifierDllUninitialize(VOID)
{
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) g_self = (HMODULE)h;  // только запоминаем handle
    return TRUE;  // никакой работы в раннем контексте
}
