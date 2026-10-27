#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace MatterLeastUpdate {

inline std::wstring join(const std::wstring& base, const std::wstring& name)
{
    if (base.empty()) return name;
    return base.back() == L'\\' ? base + name : base + L"\\" + name;
}

inline std::wstring parent(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

inline std::wstring modulePath()
{
    std::wstring result(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, result.data(), DWORD(result.size()));
    if (!n || n >= result.size()) return {};
    result.resize(n);
    return result;
}

inline bool validIdentity(const std::wstring& identity)
{
    if (identity.empty() || identity.size() > 90
        || identity == L"." || identity == L"..") return false;
    return std::all_of(identity.begin(), identity.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_';
    });
}

inline bool regularFile(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES
        && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

inline std::wstring appDir(const std::wstring& root, const std::wstring& version)
{
    return join(join(join(root, L"versions"), version), L"MatterLeast");
}

inline std::wstring appExe(const std::wstring& root, const std::wstring& version)
{
    return join(join(appDir(root, version), L"bin"), L"matterleast.exe");
}

inline std::wstring updaterExe(const std::wstring& root, const std::wstring& version)
{
    return join(join(appDir(root, version), L"bin"), L"MatterLeastUpdater.exe");
}

inline bool completeVersion(const std::wstring& root, const std::wstring& version)
{
    return validIdentity(version) && regularFile(appExe(root, version))
        && regularFile(updaterExe(root, version));
}

inline std::wstring trim(std::wstring value)
{
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    return value;
}

inline std::wstring readPointer(const std::wstring& root, const wchar_t* pointer)
{
    const std::wstring filePath = join(root, pointer);
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char bytes[256] {};
    DWORD actual = 0;
    const BOOL ok = ReadFile(file, bytes, sizeof(bytes) - 1, &actual, nullptr);
    CloseHandle(file);
    if (!ok || !actual || actual >= sizeof(bytes)) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          bytes, int(actual), nullptr, 0);
    if (length <= 0) return {};
    std::wstring decoded(size_t(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                            int(actual), decoded.data(), length)) return {};
    decoded = trim(std::move(decoded));
    return validIdentity(decoded) ? decoded : std::wstring();
}

inline bool atomicPointer(const std::wstring& root, const wchar_t* filename,
                          const std::wstring& value)
{
    if (!validIdentity(value)) return false;
    const std::wstring destination = join(root, filename);
    const std::wstring temporary = destination + L".new";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), int(value.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string bytes(size_t(std::max(0, size)), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                      int(value.size()), bytes.data(), size, nullptr, nullptr);
    bytes.push_back('\n');
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

inline std::wstring quote(const std::wstring& arg)
{
    std::wstring result = L"\"";
    size_t slashCount = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') { ++slashCount; continue; }
        if (c == L'"') {
            result.append(2 * slashCount + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(slashCount, L'\\');
            result.push_back(c);
        }
        slashCount = 0;
    }
    result.append(slashCount * 2, L'\\');
    return result + L'"';
}

inline std::vector<std::wstring> arguments()
{
    int argc = 0;
    LPWSTR* words = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> output;
    if (words) {
        for (int i = 1; i < argc; ++i) output.emplace_back(words[i]);
        LocalFree(words);
    }
    return output;
}

inline bool launch(const std::wstring& binary, const std::vector<std::wstring>& args,
                   PROCESS_INFORMATION* process)
{
    std::wstring line = quote(binary);
    for (const auto& arg : args) line += L" " + quote(arg);
    std::vector<wchar_t> writable(line.begin(), line.end());
    writable.push_back(0);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    return CreateProcessW(binary.c_str(), writable.data(), nullptr, nullptr,
        FALSE, 0, nullptr, parent(binary).c_str(), &startup, process);
}

inline void showError(const std::wstring& message)
{
    MessageBoxW(nullptr, message.c_str(), L"MatterLeast Update",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
} // namespace MatterLeastUpdate
