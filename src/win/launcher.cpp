#ifdef _WIN32

// shellapi.h relies on declarations from windows.h; keep this order.
// clang-format off
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include "launcher_config.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace {

std::wstring executablePath()
{
    std::vector<wchar_t> buffer(32768);
    const DWORD          length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    return length > 0 && length < buffer.size() ? std::wstring(buffer.data(), length) : std::wstring();
}

#ifdef ANYKEEP_DEVEL
std::wstring environmentValue(const wchar_t *name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0)
        return { };
    std::vector<wchar_t> buffer(required);
    const DWORD          copied = GetEnvironmentVariableW(name, buffer.data(), DWORD(buffer.size()));
    return copied > 0 && copied < buffer.size() ? std::wstring(buffer.data(), copied) : std::wstring();
}
#endif

std::wstring parentDirectory(const std::wstring &path)
{
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring joinPath(const std::wstring &left, const std::wstring &right)
{
    if (left.empty())
        return right;
    if (left.back() == L'\\' || left.back() == L'/')
        return left + right;
    return left + L"\\" + right;
}

std::wstring trim(std::wstring value)
{
    while (!value.empty() && std::iswspace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back()))
        value.pop_back();
    return value;
}

bool safeVersion(const std::wstring &version)
{
    if (version.empty() || version.size() > 80 || version == L"." || version == L"..")
        return false;
    return std::all_of(version.begin(), version.end(), [](wchar_t ch) {
        const bool asciiAlphaNumeric
            = (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
        return asciiAlphaNumeric || ch == L'.' || ch == L'-' || ch == L'_' || ch == L'+';
    });
}

bool writeAll(HANDLE file, const void *data, DWORD size)
{
    const auto *bytes        = static_cast<const unsigned char *>(data);
    DWORD       writtenTotal = 0;
    while (writtenTotal < size) {
        DWORD written = 0;
        if (!WriteFile(file, bytes + writtenTotal, size - writtenTotal, &written, nullptr) || written == 0)
            return false;
        writtenTotal += written;
    }
    return true;
}

bool atomicWriteText(const std::wstring &path, const std::wstring &value)
{
    const std::wstring temporary = path + L".new";
    HANDLE             file      = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    const int byteCount
        = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(std::max(byteCount, 0)), '\0');
    if (byteCount > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), int(value.size()), utf8.data(), byteCount, nullptr, nullptr);
    utf8.push_back('\n');
    const bool written = writeAll(file, utf8.data(), DWORD(utf8.size())) && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool readSmallTextFile(const std::wstring &path, std::wstring *value)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    char       buffer[512] { };
    DWORD      read = 0;
    const BOOL ok   = ReadFile(file, buffer, DWORD(sizeof(buffer) - 1), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0)
        return false;

    int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, int(read), nullptr, 0);
    if (wideLength <= 0)
        return false;
    std::wstring converted(size_t(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, int(read), converted.data(), wideLength);
    *value = trim(std::move(converted));
    return true;
}

std::wstring quoteArgument(const std::wstring &argument)
{
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring result      = L"\"";
    size_t       backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool resolveInstalledVersion(const std::wstring &root, const std::wstring &pointerName, std::wstring *version,
                             std::wstring *application, std::wstring *versionDirectory)
{
    std::wstring candidate;
    if (!readSmallTextFile(joinPath(root, pointerName), &candidate) || !safeVersion(candidate))
        return false;
    const std::wstring directory  = joinPath(joinPath(root, L"versions"), candidate);
    const std::wstring executable = joinPath(directory, L"anykeep.exe");
    if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    *version          = std::move(candidate);
    *application      = executable;
    *versionDirectory = directory;
    return true;
}

void showLaunchError(const std::wstring &detail)
{
    const std::wstring message = L"AnyKeep could not start the installed application.\n\n" + detail;
    MessageBoxW(nullptr, message.c_str(), L"AnyKeep", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::wstring launcher = executablePath();
#ifdef ANYKEEP_DEVEL
    // Qt Creator runs the launcher from the build tree. Let it exercise a
    // disposable versioned installation without copying the launcher there.
    const std::wstring updateRoot = environmentValue(L"ANYKEEP_UPDATE_ROOT");
    const std::wstring root       = updateRoot.empty() ? parentDirectory(launcher) : updateRoot;
#else
    const std::wstring root = parentDirectory(launcher);
#endif
    if (root.empty()) {
        showLaunchError(L"The launcher location could not be determined.");
        return 1;
    }

    std::wstring version;
    std::wstring application;
    std::wstring versionDirectory;
    const bool   currentResolved
        = resolveInstalledVersion(root, L"current.version", &version, &application, &versionDirectory);
    if (!currentResolved
        && !resolveInstalledVersion(root, L"previous.version", &version, &application, &versionDirectory)) {
        const std::wstring initialVersion     = ANYKEEP_INITIAL_VERSION_W;
        const std::wstring initialDirectory   = joinPath(joinPath(root, L"versions"), initialVersion);
        const std::wstring initialApplication = joinPath(initialDirectory, L"anykeep.exe");
        if (!safeVersion(initialVersion) || GetFileAttributesW(initialApplication.c_str()) == INVALID_FILE_ATTRIBUTES) {
            showLaunchError(L"No version pointer or complete initial installation could be found.");
            return 2;
        }
        version          = initialVersion;
        application      = initialApplication;
        versionDirectory = initialDirectory;
    }
    if (!currentResolved)
        atomicWriteText(joinPath(root, L"current.version"), version);

    int          argc        = 0;
    LPWSTR      *argv        = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring commandLine = quoteArgument(application);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            commandLine.push_back(L' ');
            commandLine += quoteArgument(argv[i]);
        }
        LocalFree(argv);
    }

    STARTUPINFOW startup { };
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION  process { };
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    if (!CreateProcessW(application.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        versionDirectory.c_str(), &startup, &process)) {
        showLaunchError(L"Windows error " + std::to_wstring(GetLastError()) + L" while starting:\n" + application);
        return 4;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

#endif
