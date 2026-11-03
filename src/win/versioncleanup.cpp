#ifdef _WIN32

// clang-format off
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

#include "versioncleanup.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>

namespace AnyKeepWin {
namespace {

namespace fs = std::filesystem;
constexpr auto VersionGrace = std::chrono::hours(24);

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return std::towlower(ch); });
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

bool safeVersion(const std::wstring &name)
{
    return !name.empty() && name.size() <= 80 && name != L"." && name != L".."
        && std::all_of(name.begin(), name.end(), [](wchar_t ch) {
               return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z')
                   || (ch >= L'a' && ch <= L'z') || ch == L'.' || ch == L'-'
                   || ch == L'_' || ch == L'+';
           });
}

std::wstring readVersion(const fs::path &file)
{
    std::ifstream input(file, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line.size() > 80)
        return {};
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    std::wstring name(line.begin(), line.end());
    return safeVersion(name) ? name : std::wstring();
}

void log(const fs::path &root, const std::string &message)
{
    std::error_code ec;
    fs::create_directories(root / L"update-logs", ec);
    if (!ec) {
        std::ofstream output(root / L"update-logs" / L"cleanup.log", std::ios::app);
        output << message << '\n';
    }
}

bool isReparsePoint(const fs::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool safeTree(const fs::path &path)
{
    if (isReparsePoint(path))
        return false;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(path, ec), end; it != end && !ec; it.increment(ec)) {
        if (isReparsePoint(it->path()))
            return false;
    }
    return !ec;
}

bool runningVersions(std::set<std::wstring> *running)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool ok = Process32FirstW(snapshot, &entry);
    if (ok) {
        do {
            const std::wstring name = lower(entry.szExeFile);
            if (name != L"anykeep.exe" && name != L"anykeepupdater.exe")
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                ok = false;
                break;
            }
            wchar_t filename[32768] {};
            DWORD length = DWORD(32768);
            const bool queried = QueryFullProcessImageNameW(process, 0, filename, &length) != 0;
            CloseHandle(process);
            if (!queried) {
                ok = false;
                break;
            }
            running->insert(lower(std::wstring(filename, length)));
        } while (Process32NextW(snapshot, &entry));
        if (ok && GetLastError() != ERROR_NO_MORE_FILES)
            ok = false;
    }
    CloseHandle(snapshot);
    return ok;
}

} // namespace

MaintenanceLock::MaintenanceLock(const std::wstring &root)
{
    ULONGLONG hash = 14695981039346656037ULL;
    for (const wchar_t ch : lower(root)) {
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
    if (!mutex_)
        return false;
    const DWORD result = WaitForSingleObject(mutex_, timeoutMs);
    locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    return locked_;
}

void MaintenanceLock::release()
{
    if (locked_) {
        ReleaseMutex(mutex_);
        locked_ = false;
    }
}

void cleanupInstalledVersions(const std::wstring &rootName, const std::wstring &msiVersion)
{
    const fs::path root(rootName), versions = root / L"versions";
    if (!safeVersion(msiVersion) || isReparsePoint(root) || isReparsePoint(versions))
        return;
    const std::wstring current = readVersion(root / L"current.version");
    const std::wstring previous = readVersion(root / L"previous.version");
    // Leave the old installations alone until rollback state is unambiguous.
    if (current.empty() || previous.empty())
        return;

    std::error_code ec;
    if (!fs::is_regular_file(versions / current / L"anykeep.exe", ec) || ec
        || !fs::is_regular_file(versions / previous / L"anykeep.exe", ec) || ec)
        return;
    if (fs::exists(root / L"staging" / L"prepared.json", ec) || ec)
        return; // A prepared update could still be waiting to be applied.

    std::set<std::wstring> running;
    if (!runningVersions(&running))
        return;

    for (fs::directory_iterator it(versions, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path path = it->path();
        const std::wstring name = path.filename().wstring();
        if (!safeVersion(name) || name.front() == L'.' || lower(name) == lower(current) || lower(name) == lower(previous)
            || lower(name) == lower(msiVersion) || isReparsePoint(path)
            || !it->is_directory(ec) || ec)
            continue;
        const auto modified = fs::last_write_time(path, ec);
        if (ec || fs::file_time_type::clock::now() - modified < VersionGrace)
            continue;
        const fs::path executable = (path / L"anykeep.exe").lexically_normal();
        const fs::path updater = (path / L"AnyKeepUpdater.exe").lexically_normal();
        if (running.contains(lower(executable.wstring())) || running.contains(lower(updater.wstring()))
            || !safeTree(path))
            continue;
        const std::uintmax_t removed = fs::remove_all(path, ec);
        const std::string version(name.begin(), name.end());
        if (ec)
            log(root, "Could not delete obsolete version " + version + ": " + ec.message());
        else
            log(root, "Removed obsolete version " + version + " (" + std::to_string(removed) + " entries)");
    }
}

} // namespace AnyKeepWin

#endif
