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

// Путь к актуальной DLL темы. При смене версии правится здесь (или ребилд
// из build\build_loader.bat, который подставляет путь).
static const WCHAR kThemeDllPath[] =
    L"D:\\1CTheme\\Настройка темы в конфигураторе 1С\\builds\\ThemeHook3_v55.dll";

extern "C" __declspec(dllexport) BOOL WINAPI VerifierDllInitialize(DWORD dwReason)
{
    (void)dwReason;
    HMODULE h = LoadLibraryW(kThemeDllPath);
    // TRUE в любом случае: даже если тема не найдена/не загрузилась,
    // конфигуратор обязан запуститься (тема - опция, не блокер).
    (void)h;
    return TRUE;
}

extern "C" __declspec(dllexport) VOID WINAPI VerifierDllUninitialize(VOID)
{
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
{
    return TRUE;  // никакой работы в раннем контексте
}
