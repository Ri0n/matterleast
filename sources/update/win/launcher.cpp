#include "WinUpdateHelpers.h"
#include "LauncherVersion.h"

using namespace MatterLeastUpdate;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::wstring root = parent(modulePath());
    if (root.empty()) return 1;

    // Repair a broken pointer before starting.  The previous known-good
    // version always wins over the original bundled version.
    std::wstring current = readPointer(root, L"current.version");
    if (!completeVersion(root, current)) {
        current = readPointer(root, L"previous.version");
        if (!completeVersion(root, current)) {
            current = MATTERLEAST_INSTALL_VERSION_W;
            if (!completeVersion(root, current)) {
                showError(L"No complete installed version is available.");
                return 2;
            }
        }
        if (!atomicPointer(root, L"current.version", current)) {
            showError(L"Could not repair the active version pointer.");
            return 3;
        }
    }

    PROCESS_INFORMATION child {};
    if (!launch(appExe(root, current), arguments(), &child)) {
        // The current version may be corrupted despite an existing binary.
        const std::wstring previous = readPointer(root, L"previous.version");
        if (previous != current && completeVersion(root, previous)
            && atomicPointer(root, L"current.version", previous)
            && launch(appExe(root, previous), arguments(), &child)) {
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
            return 0;
        }
        showError(L"Unable to launch MatterLeast. Windows error "
                  + std::to_wstring(GetLastError()));
        return 4;
    }
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 0;
}
