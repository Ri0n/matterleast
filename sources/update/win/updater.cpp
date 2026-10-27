#include "WinUpdateHelpers.h"

#include <cstdlib>

using namespace MatterLeastUpdate;

namespace {
constexpr DWORD StartupTimeoutMs = 120000;
constexpr DWORD OldProcessTimeoutMs = 300000;

std::wstring argument(const std::vector<std::wstring>& args, const wchar_t* name)
{
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == name) return args[i + 1];
    return {};
}

bool waitForOldProcess(DWORD pid)
{
    if (!pid) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD status = WaitForSingleObject(process, OldProcessTimeoutMs);
    CloseHandle(process);
    return status == WAIT_OBJECT_0;
}

void restart(const std::wstring& root)
{
    PROCESS_INFORMATION child {};
    if (launch(join(root, L"MatterLeastLauncher.exe"), {}, &child)) {
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const auto args = arguments();
    const std::wstring root = argument(args, L"--root");
    const std::wstring target = argument(args, L"--target");
    const std::wstring pidString = argument(args, L"--wait-pid");
    if (root.empty() || !validIdentity(target) || pidString.empty())
        return 10;

    // Do not let an arbitrary command line turn the updater into a
    // generic pointer-writing tool: it must live inside the target version.
    std::wstring own = modulePath();
    std::wstring expected = updaterExe(root, target);
    // Qt supplies C:/... paths while GetModuleFileNameW uses C:\\... .
    // They identify the same file; normalize before checking provenance.
    std::replace(own.begin(), own.end(), L'/', L'\\');
    std::replace(expected.begin(), expected.end(), L'/', L'\\');
    if (_wcsicmp(own.c_str(), expected.c_str()) != 0)
        return 11;
    wchar_t* end = nullptr;
    const unsigned long pid = std::wcstoul(pidString.c_str(), &end, 10);
    if (!end || *end || !pid || !waitForOldProcess(DWORD(pid)))
        return 12;

    const std::wstring previous = readPointer(root, L"current.version");
    if (!completeVersion(root, previous) || !completeVersion(root, target)
        || previous == target) return 13;

    if (!atomicPointer(root, L"previous.version", previous))
        return 14;
    if (!atomicPointer(root, L"current.version", target)) {
        restart(root);
        return 15;
    }

    const std::wstring staging = join(root, L"staging");
    CreateDirectoryW(staging.c_str(), nullptr);
    const std::wstring marker = join(staging,
        L"startup-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64()) + L".ok");
    DeleteFileW(marker.c_str());

    PROCESS_INFORMATION newApp {};
    const bool started = launch(appExe(root, target),
                                {L"--matterleast-update-probe-file", marker}, &newApp);
    bool confirmed = false, normalExit = false;
    if (started) {
        CloseHandle(newApp.hThread);
        const ULONGLONG deadline = GetTickCount64() + StartupTimeoutMs;
        while (GetTickCount64() < deadline) {
            if (regularFile(marker)) { confirmed = true; break; }
            const DWORD wait = WaitForSingleObject(newApp.hProcess, 250);
            if (wait == WAIT_OBJECT_0) {
                DWORD code = STILL_ACTIVE;
                if (GetExitCodeProcess(newApp.hProcess, &code) && code == 0)
                    normalExit = true;
                break;
            }
            if (wait != WAIT_TIMEOUT) break;
        }
        if (regularFile(marker)) confirmed = true;
        if (!confirmed && !normalExit
            && WaitForSingleObject(newApp.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(newApp.hProcess, 18);
            WaitForSingleObject(newApp.hProcess, 10000);
        }
        CloseHandle(newApp.hProcess);
    }
    DeleteFileW(marker.c_str());

    if (confirmed || normalExit) {
        // The updater keeps previous.version and its binaries for rollback
        // across the next launch.  Only temporary update state is removed.
        DeleteFileW(join(staging, L"prepared.json").c_str());
        return 0;
    }

    // A failed launch, early abnormal exit or missing health probe is an
    // unsuccessful update. Restore the exact previous version and restart it.
    if (!atomicPointer(root, L"current.version", previous)) {
        PROCESS_INFORMATION oldApp {};
        if (launch(appExe(root, previous), {}, &oldApp)) {
            CloseHandle(oldApp.hThread);
            CloseHandle(oldApp.hProcess);
        }
        return 17;
    }
    restart(root);
    return 16;
}
