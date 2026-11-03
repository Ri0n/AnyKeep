#ifdef _WIN32

// clang-format off
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

#include "versioncleanup.h"
#include "versionretention.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <cstdio>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace AnyKeepWin {
namespace {

constexpr ULONGLONG TicksPerSecond = 10000000ULL;
constexpr ULONGLONG VersionGrace = 24ULL * 60 * 60 * TicksPerSecond;
constexpr ULONGLONG StageGrace = 7ULL * 24 * 60 * 60 * TicksPerSecond;

std::wstring join(const std::wstring &left, const std::wstring &right)
{
    return left + L"\\" + right;
}

std::wstring lowercase(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return std::towlower(ch); });
    std::replace(text.begin(), text.end(), L'/', L'\\');
    return text;
}

bool isDirectoryWithoutReparse(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

void logCleanup(const std::wstring &root, const std::wstring &message)
{
    const std::wstring logDir = join(root, L"update-logs");
    CreateDirectoryW(logDir.c_str(), nullptr);
    const std::wstring filename = join(logDir, L"cleanup.log");
    HANDLE file = CreateFileW(filename.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME now {};
    GetLocalTime(&now);
    wchar_t timestamp[64] {};
    swprintf_s(timestamp, L"%04u-%02u-%02u %02u:%02u:%02u ",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::wstring entry = std::wstring(timestamp) + message + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, entry.data(), int(entry.size()), nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        std::string bytes(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, entry.data(), int(entry.size()), bytes.data(), n, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr);
    }
    CloseHandle(file);
}

bool readText(const std::wstring &path, std::string *result, DWORD maximum = 65536)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER length {};
    const bool reasonable = GetFileSizeEx(file, &length) && length.QuadPart > 0
        && length.QuadPart <= maximum;
    if (!reasonable) {
        CloseHandle(file);
        return false;
    }
    result->resize(size_t(length.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, result->data(), DWORD(result->size()), &read, nullptr)
        && read == result->size();
    CloseHandle(file);
    return ok;
}

std::wstring trim(std::wstring text)
{
    while (!text.empty() && std::iswspace(text.front()))
        text.erase(text.begin());
    while (!text.empty() && std::iswspace(text.back()))
        text.pop_back();
    return text;
}

bool readVersionPointer(const std::wstring &root, const wchar_t *name, std::wstring *result)
{
    std::string bytes;
    if (!readText(join(root, name), &bytes, 512))
        return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           int(bytes.size()), nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring text(size_t(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                             int(bytes.size()), text.data(), length))
        return false;
    *result = trim(std::move(text));
    return safeVersionName(*result);
}

bool readPrepared(const std::wstring &root, std::wstring *result)
{
    std::string json;
    if (!readText(join(join(root, L"staging"), L"prepared.json"), &json))
        return false;
    const std::regex pattern(R"regex("version"\s*:\s*"([A-Za-z0-9._+-]+)")regex");
    std::smatch match;
    if (!std::regex_search(json, match, pattern))
        return false;
    result->assign(match[1].first, match[1].second);
    return safeVersionName(*result);
}

struct Entry {
    std::wstring name;
    WIN32_FIND_DATAW findData {};
};

bool listDirectories(const std::wstring &root, std::vector<Entry> *result)
{
    WIN32_FIND_DATAW found {};
    HANDLE handle = FindFirstFileW(join(root, L"*").c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            result->push_back({ found.cFileName, found });
    } while (FindNextFileW(handle, &found));
    const DWORD error = GetLastError();
    FindClose(handle);
    return error == ERROR_NO_MORE_FILES;
}

ULONGLONG ticks(const FILETIME &value)
{
    ULARGE_INTEGER bits {};
    bits.LowPart = value.dwLowDateTime;
    bits.HighPart = value.dwHighDateTime;
    return bits.QuadPart;
}

bool expired(const WIN32_FIND_DATAW &data, ULONGLONG now, ULONGLONG age)
{
    const ULONGLONG modified = std::max(ticks(data.ftCreationTime), ticks(data.ftLastWriteTime));
    return now > modified && now - modified >= age;
}

bool hasReparsePoint(const std::wstring &path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return false;

    WIN32_FIND_DATAW item {};
    HANDLE handle = FindFirstFileW(join(path, L"*").c_str(), &item);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError() != ERROR_FILE_NOT_FOUND;
    bool unsafe = false;
    do {
        if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0)
            continue;
        if ((item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                && hasReparsePoint(join(path, item.cFileName)))) {
            unsafe = true;
            break;
        }
    } while (FindNextFileW(handle, &item));
    if (!unsafe && GetLastError() != ERROR_NO_MORE_FILES)
        unsafe = true;
    FindClose(handle);
    return unsafe;
}

bool deleteTree(const std::wstring &path)
{
    // Recheck each child after the preflight scan: never traverse a junction
    // that appeared during cleanup, even if this leaves a partial old tree.
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)
        || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
    WIN32_FIND_DATAW item {};
    HANDLE handle = FindFirstFileW(join(path, L"*").c_str(), &item);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND && RemoveDirectoryW(path.c_str());
    bool ok = true;
    do {
        if (wcscmp(item.cFileName, L".") == 0 || wcscmp(item.cFileName, L"..") == 0)
            continue;
        const std::wstring child = join(path, item.cFileName);
        const DWORD actual = GetFileAttributesW(child.c_str());
        if (actual == INVALID_FILE_ATTRIBUTES || (actual & FILE_ATTRIBUTE_REPARSE_POINT)) {
            ok = false;
            break;
        }
        if (actual & FILE_ATTRIBUTE_DIRECTORY) {
            if (!deleteTree(child)) {
                ok = false;
                break;
            }
        } else if (!DeleteFileW(child.c_str())) {
            ok = false;
            break;
        }
    } while (FindNextFileW(handle, &item));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES)
        ok = false;
    FindClose(handle);
    return ok && RemoveDirectoryW(path.c_str());
}

bool runningVersions(const std::wstring &root, std::unordered_set<std::wstring> *result)
{
    const std::wstring prefix = lowercase(join(join(root, L"versions"), L""));
    const std::wstring normalizedPrefix = prefix.back() == L'\\' ? prefix : prefix + L'\\';
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W process {};
    process.dwSize = sizeof(process);
    bool ok = Process32FirstW(snapshot, &process);
    if (!ok) {
        CloseHandle(snapshot);
        return false;
    }
    do {
        const std::wstring image = lowercase(process.szExeFile);
        if (image != L"anykeep.exe" && image != L"anykeepupdater.exe")
            continue;
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.th32ProcessID);
        if (!handle) {
            ok = false;
            break;
        }
        wchar_t path[32768] {};
        DWORD capacity = DWORD(std::size(path));
        const BOOL queried = QueryFullProcessImageNameW(handle, 0, path, &capacity);
        CloseHandle(handle);
        if (!queried) {
            ok = false;
            break;
        }
        const std::wstring executable = lowercase(std::wstring(path, capacity));
        if (executable.compare(0, normalizedPrefix.size(), normalizedPrefix) != 0)
            continue;
        const std::wstring relative = executable.substr(normalizedPrefix.size());
        const size_t slash = relative.find(L'\\');
        if (slash != std::wstring::npos)
            result->insert(relative.substr(0, slash));
    } while (Process32NextW(snapshot, &process));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES)
        ok = false;
    CloseHandle(snapshot);
    return ok;
}

} // namespace

MaintenanceLock::MaintenanceLock(const std::wstring &root)
{
    // A per-install mutex permits independent development installations and
    // serializes launcher cleanup against the updater's pointer changes.
    ULONGLONG hash = 14695981039346656037ULL;
    for (wchar_t ch : lowercase(root)) {
        hash ^= static_cast<unsigned int>(ch);
        hash *= 1099511628211ULL;
    }
    wchar_t name[96] {};
    swprintf_s(name, L"Local\\AnyKeepVersionMaintenance-%016llX", hash);
    mutex_ = CreateMutexW(nullptr, FALSE, name);
}

MaintenanceLock::~MaintenanceLock()
{
    release();
    if (mutex_)
        CloseHandle(mutex_);
}

bool MaintenanceLock::acquire(DWORD timeoutMs)
{
    if (locked_)
        return true;
    if (!mutex_)
        return false;
    const DWORD wait = WaitForSingleObject(mutex_, timeoutMs);
    locked_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
    return locked_;
}

void MaintenanceLock::release()
{
    if (locked_) {
        ReleaseMutex(mutex_);
        locked_ = false;
    }
}

void cleanupInstalledVersions(const std::wstring &root, const std::wstring &msiVersion)
{
    if (!safeVersionName(msiVersion) || !isDirectoryWithoutReparse(root))
        return;
    const std::wstring versions = join(root, L"versions");
    if (!isDirectoryWithoutReparse(versions))
        return;

    VersionRetention retention;
    retention.msiInstalled = msiVersion;
    retention.pointersValid = readVersionPointer(root, L"current.version", &retention.current);
    if (!retention.pointersValid) {
        logCleanup(root, L"Skipped: current.version is missing or invalid");
        return;
    }

    const DWORD previousAttrs = GetFileAttributesW(join(root, L"previous.version").c_str());
    if (previousAttrs != INVALID_FILE_ATTRIBUTES) {
        if (!readVersionPointer(root, L"previous.version", &retention.previous)) {
            logCleanup(root, L"Skipped: previous.version is invalid");
            return;
        }
    }
    // A previous version is required before removing any version. This also
    // preserves the first installation and a fallback after pointer damage.
    if (retention.previous.empty()) {
        return;
    }

    const DWORD preparedAttrs = GetFileAttributesW(join(join(root, L"staging"), L"prepared.json").c_str());
    const DWORD preparedError = preparedAttrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    retention.preparedValid = preparedAttrs == INVALID_FILE_ATTRIBUTES
        && (preparedError == ERROR_FILE_NOT_FOUND || preparedError == ERROR_PATH_NOT_FOUND);
    if (!retention.preparedValid && !readPrepared(root, &retention.prepared)) {
        logCleanup(root, L"Skipped: prepared.json is unreadable");
        return;
    }
    retention.preparedValid = true;

    std::unordered_set<std::wstring> inUse;
    if (!runningVersions(root, &inUse)) {
        logCleanup(root, L"Skipped: unable to enumerate running AnyKeep processes");
        return;
    }

    FILETIME currentTime {};
    GetSystemTimeAsFileTime(&currentTime);
    const ULONGLONG now = ticks(currentTime);
    std::vector<Entry> entries;
    if (!listDirectories(versions, &entries)) {
        logCleanup(root, L"Skipped: unable to enumerate versions directory");
        return;
    }

    for (const Entry &entry : entries) {
        const bool temporary = entry.name.size() > 5 && entry.name[0] == L'.'
            && entry.name.ends_with(L".tmp") && safeVersionName(entry.name.substr(1, entry.name.size() - 5));
        const bool normal = safeVersionName(entry.name);
        if (!normal && !temporary)
            continue;
        const std::wstring path = join(versions, entry.name);
        if (temporary) {
            // Only the matching prepared version owns its temporary sibling.
            const std::wstring temporaryVersion = entry.name.substr(1, entry.name.size() - 5);
            if (sameVersion(temporaryVersion, retention.prepared)
                || !expired(entry.findData, now, StageGrace) || hasReparsePoint(path))
                continue;
        } else {
            VersionCandidate candidate { entry.name, expired(entry.findData, now, VersionGrace),
                                         inUse.contains(lowercase(entry.name)),
                                         (entry.findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 };
            if (!canRemoveVersion(candidate, retention) || hasReparsePoint(path))
                continue;
        }
        if (deleteTree(path))
            logCleanup(root, L"Removed obsolete " + std::wstring(temporary ? L"temporary " : L"")
                                   + L"version directory: " + entry.name);
        else
            logCleanup(root, L"Could not fully remove obsolete version directory: " + entry.name
                                   + L", Windows error " + std::to_wstring(GetLastError()));
    }

    const std::wstring staging = join(root, L"staging");
    if (!isDirectoryWithoutReparse(staging))
        return;
    std::vector<Entry> stages;
    if (!listDirectories(staging, &stages))
        return;
    for (const Entry &entry : stages) {
        if (!safeVersionName(entry.name) || sameVersion(entry.name, retention.prepared)
            || !expired(entry.findData, now, StageGrace))
            continue;
        const std::wstring path = join(staging, entry.name);
        if (hasReparsePoint(path))
            continue;
        if (deleteTree(path))
            logCleanup(root, L"Removed stale update staging directory: " + entry.name);
        else
            logCleanup(root, L"Could not fully remove stale staging directory: " + entry.name
                                   + L", Windows error " + std::to_wstring(GetLastError()));
    }
}

void spawnCleanupHelper(const std::wstring &root)
{
    const std::wstring launcher = join(root, L"AnyKeepLauncher.exe");
    const std::wstring commandLine = L"\"" + launcher + L"\" --cleanup";
    std::vector<wchar_t> command(commandLine.begin(), commandLine.end());
    command.push_back(L'\0');
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

} // namespace AnyKeepWin

#endif
