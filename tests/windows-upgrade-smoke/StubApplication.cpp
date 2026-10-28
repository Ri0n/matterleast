// Small Win32 replacement for the user-facing UI, enabling automated
// launcher/rollback checks without starting a chat client or logging in.
#include <windows.h>
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
#ifdef FAIL_STARTUP
    return 19;
#else
    int argc = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!args) return 20;
    std::wstring probe;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(args[i]) == L"--matterleast-update-probe-file")
            probe = args[i + 1];
    }
    LocalFree(args);
    if (!probe.empty()) {
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 21;
        DWORD written = 0;
        const BOOL success = WriteFile(file, "ok", 2, &written, nullptr);
        CloseHandle(file);
        if (!success || written != 2) return 22;
    }
    return 0;
#endif
}
