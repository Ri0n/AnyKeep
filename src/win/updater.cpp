#ifdef _WIN32

// shellapi.h relies on declarations from windows.h; keep this order.
// clang-format off
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr DWORD   StartupProbeTimeoutMs = 120000;
constexpr wchar_t RollbackFileName[]    = L"rollback.json";

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

bool readSmallTextFile(const std::wstring &path, std::wstring *value)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char       buffer[512] {};
    DWORD      read = 0;
    const BOOL ok   = ReadFile(file, buffer, DWORD(sizeof(buffer) - 1), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0)
        return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, int(read), nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring converted(size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buffer, int(read), converted.data(), length);
    *value = trim(std::move(converted));
    return true;
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

void writeRollbackResult(const std::wstring &root, const std::wstring &version, const std::wstring &previousVersion,
                         const std::wstring &reason)
{
    const std::wstring staging = joinPath(root, L"staging");
    CreateDirectoryW(staging.c_str(), nullptr);
    const std::wstring result = L"{\"schema\":1,\"version\":\"" + version + L"\",\"previousVersion\":\""
        + previousVersion + L"\",\"reason\":\"" + reason + L"\"}\n";
    atomicWriteText(joinPath(staging, RollbackFileName), result);
}

void appendLog(const std::wstring &root, const std::wstring &message)
{
    const std::wstring logDirectory = joinPath(root, L"update-logs");
    CreateDirectoryW(logDirectory.c_str(), nullptr);
    const std::wstring logPath = joinPath(logDirectory, L"updater.log");
    HANDLE file = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME time {};
    GetLocalTime(&time);
    wchar_t prefix[64] {};
    swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u ", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond);
    const std::wstring line = std::wstring(prefix) + message + L"\r\n";
    const int   bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(std::max(bytes, 0)), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), DWORD(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

bool waitForProcess(DWORD pid)
{
    if (pid == 0)
        return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD result = WaitForSingleObject(process, INFINITE);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}

bool launchProcess(const std::wstring &application, const std::vector<std::wstring> &arguments,
                   const std::wstring &workingDirectory, PROCESS_INFORMATION *process)
{
    std::wstring commandLine = quoteArgument(application);
    for (const auto &argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    return CreateProcessW(application.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                          workingDirectory.c_str(), &startup, process);
}

std::wstring argumentValue(const std::vector<std::wstring> &arguments, const std::wstring &name)
{
    for (size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == name)
            return arguments[i + 1];
    }
    return {};
}

bool hasArgument(const std::vector<std::wstring> &arguments, const std::wstring &name)
{
    return std::find(arguments.begin(), arguments.end(), name) != arguments.end();
}

void restartThroughLauncher(const std::wstring &root)
{
    const std::wstring  launcher = joinPath(root, L"AnyKeepLauncher.exe");
    PROCESS_INFORMATION process {};
    if (!launchProcess(launcher, {}, root, &process)) {
        appendLog(root,
                  L"Could not restart AnyKeep through the launcher; Windows error " + std::to_wstring(GetLastError()));
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

void clearPreparedState(const std::wstring &root, const std::wstring &version)
{
    const std::wstring staging = joinPath(root, L"staging");
    DeleteFileW(joinPath(staging, L"prepared.json").c_str());
    RemoveDirectoryW(joinPath(staging, version).c_str());
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int                       argc         = 0;
    LPWSTR                   *rawArguments = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> arguments;
    if (rawArguments) {
        for (int i = 1; i < argc; ++i)
            arguments.emplace_back(rawArguments[i]);
        LocalFree(rawArguments);
    }

    const std::wstring root    = argumentValue(arguments, L"--root");
    const std::wstring version = argumentValue(arguments, L"--version");
    const std::wstring pidText = argumentValue(arguments, L"--wait-pid");
    const bool         restart = hasArgument(arguments, L"--restart");
    if (root.empty() || !safeVersion(version))
        return 10;

    const DWORD oldPid = pidText.empty() ? 0 : DWORD(std::wcstoul(pidText.c_str(), nullptr, 10));
    appendLog(root, L"Preparing switch to " + version + L"; waiting for process " + std::to_wstring(oldPid));
    if (!waitForProcess(oldPid)) {
        appendLog(root, L"Failed while waiting for the old process");
        return 11;
    }

    const std::wstring pointerPath = joinPath(root, L"current.version");
    std::wstring       previousVersion;
    if (!readSmallTextFile(pointerPath, &previousVersion) || !safeVersion(previousVersion)) {
        appendLog(root, L"The existing current.version pointer is invalid");
        restartThroughLauncher(root);
        return 12;
    }

    const std::wstring newDirectory   = joinPath(joinPath(root, L"versions"), version);
    const std::wstring newApplication = joinPath(newDirectory, L"anykeep.exe");
    if (GetFileAttributesW(newApplication.c_str()) == INVALID_FILE_ATTRIBUTES) {
        appendLog(root, L"The new application executable is missing");
        restartThroughLauncher(root);
        return 13;
    }

    if (!atomicWriteText(joinPath(root, L"previous.version"), previousVersion)
        || !atomicWriteText(pointerPath, version)) {
        appendLog(root, L"Could not atomically switch current.version");
        restartThroughLauncher(root);
        return 14;
    }
    appendLog(root, L"Switched current.version from " + previousVersion + L" to " + version);

    if (!restart)
        return 0;

    const std::wstring staging = joinPath(root, L"staging");
    CreateDirectoryW(staging.c_str(), nullptr);
    const std::wstring marker = joinPath(staging,
                                         L"startup-" + std::to_wstring(GetCurrentProcessId()) + L"-"
                                             + std::to_wstring(GetTickCount64()) + L".ok");
    DeleteFileW(marker.c_str());

    PROCESS_INFORMATION newProcess {};
    if (!launchProcess(newApplication, { L"--update-probe-file", marker }, newDirectory, &newProcess)) {
        appendLog(root, L"Could not launch the new application; rolling back");
        atomicWriteText(pointerPath, previousVersion);
        restartThroughLauncher(root);
        return 15;
    }
    appendLog(root, L"Started the new version; waiting for startup marker " + marker);
    CloseHandle(newProcess.hThread);

    const ULONGLONG deadline  = GetTickCount64() + StartupProbeTimeoutMs;
    bool            confirmed = false;
    bool            exited    = false;
    DWORD           exitCode  = STILL_ACTIVE;
    while (GetTickCount64() < deadline) {
        if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
            confirmed = true;
            break;
        }
        const DWORD wait = WaitForSingleObject(newProcess.hProcess, 250);
        if (wait == WAIT_OBJECT_0) {
            exited = true;
            GetExitCodeProcess(newProcess.hProcess, &exitCode);
            break;
        }
    }

    if (!confirmed && GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
        confirmed = true;

    if (confirmed) {
        clearPreparedState(root, version);
        CloseHandle(newProcess.hProcess);
        DeleteFileW(marker.c_str());
        appendLog(root, L"The new version completed the 60-second healthy-startup probe");
        return 0;
    }

    if (exited && exitCode == 0) {
        clearPreparedState(root, version);
        CloseHandle(newProcess.hProcess);
        DeleteFileW(marker.c_str());
        appendLog(root, L"The new version closed normally before the health timer elapsed; keeping it current");
        return 0;
    }

    if (!exited) {
        appendLog(root, L"The new version did not confirm startup before the timeout; terminating it for rollback");
        TerminateProcess(newProcess.hProcess, 18);
        WaitForSingleObject(newProcess.hProcess, 10000);
    } else {
        appendLog(root, L"The new version exited abnormally with code " + std::to_wstring(exitCode));
    }
    CloseHandle(newProcess.hProcess);
    DeleteFileW(marker.c_str());

    const std::wstring rollbackReason = !exited ? L"startup-timeout" : L"abnormal-exit";
    writeRollbackResult(root, version, previousVersion, rollbackReason);
    appendLog(root, L"Rolling back to " + previousVersion);
    if (!atomicWriteText(pointerPath, previousVersion)) {
        appendLog(root, L"Could not restore current.version; starting the previous executable directly");
        const std::wstring  previousDirectory   = joinPath(joinPath(root, L"versions"), previousVersion);
        const std::wstring  previousApplication = joinPath(previousDirectory, L"anykeep.exe");
        PROCESS_INFORMATION previousProcess {};
        if (launchProcess(previousApplication, {}, previousDirectory, &previousProcess)) {
            CloseHandle(previousProcess.hThread);
            CloseHandle(previousProcess.hProcess);
        }
        return 16;
    }

    restartThroughLauncher(root);
    return 17;
}

#endif
