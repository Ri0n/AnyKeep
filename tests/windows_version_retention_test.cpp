#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../src/win/versioncleanup.h"
#include "../src/win/versionretention.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

std::wstring join(const std::wstring &a, const std::wstring &b) { return a + L"\\" + b; }

void put(const std::wstring &path, const std::string &value)
{
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    f << value;
    expect(f.good(), "write test fixture");
}

bool exists(const std::wstring &path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void makeOld(const std::wstring &path)
{
    HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    expect(handle != INVALID_HANDLE_VALUE, "open directory for timestamp");
    FILETIME now {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks {};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    ticks.QuadPart -= 10ULL * 24 * 60 * 60 * 10000000ULL;
    FILETIME old { ticks.LowPart, ticks.HighPart };
    expect(SetFileTime(handle, &old, nullptr, &old) != 0, "set old timestamp");
    CloseHandle(handle);
}

void makeVersion(const std::wstring &versions, const wchar_t *name)
{
    const std::wstring dir = join(versions, name);
    expect(CreateDirectoryW(dir.c_str(), nullptr), "create version");
    put(join(dir, L"anykeep.exe"), "fixture");
    makeOld(dir);
}

void policyTests()
{
    using namespace AnyKeepWin;
    VersionRetention retention { L"4.0", L"3.0", L"1.0", L"5.0", true, true };
    const auto candidate = [](const wchar_t *name) { return VersionCandidate { name, true, false, false }; };
    expect(canRemoveVersion(candidate(L"2.0"), retention), "obsolete version eligible");
    for (const wchar_t *protectedVersion : { L"4.0", L"3.0", L"1.0", L"5.0", L"4.0" }) {
        expect(!canRemoveVersion(candidate(protectedVersion), retention), "protected version retained");
    }
    expect(!canRemoveVersion(candidate(L"../outside"), retention), "traversal rejected");
    expect(!canRemoveVersion(candidate(L"bad/name"), retention), "path separators rejected");
    expect(!canRemoveVersion({ L"2.0", false, false, false }, retention), "recent version retained");
    expect(!canRemoveVersion({ L"2.0", true, true, false }, retention), "running version retained");
    expect(!canRemoveVersion({ L"2.0", true, false, true }, retention), "reparse version retained");
    retention.pointersValid = false;
    expect(!canRemoveVersion(candidate(L"2.0"), retention), "invalid pointers stop deletion");
    retention.pointersValid = true;
    retention.preparedValid = false;
    expect(!canRemoveVersion(candidate(L"2.0"), retention), "invalid prepared state stops deletion");
    expect(sameVersion(L"4.0-RC", L"4.0-rc"), "case insensitive version match");
}

void integrationTest()
{
    wchar_t temp[MAX_PATH] {};
    expect(GetTempPathW(MAX_PATH, temp) > 0, "temporary path");
    wchar_t unique[MAX_PATH] {};
    expect(GetTempFileNameW(temp, L"akv", 0, unique), "unique root");
    DeleteFileW(unique);
    const std::wstring root(unique);
    expect(CreateDirectoryW(root.c_str(), nullptr), "make root");
    const std::wstring versions = join(root, L"versions");
    const std::wstring staging = join(root, L"staging");
    expect(CreateDirectoryW(versions.c_str(), nullptr), "versions root");
    expect(CreateDirectoryW(staging.c_str(), nullptr), "staging root");
    for (const wchar_t *v : { L"1.0", L"2.0", L"3.0", L"4.0", L"5.0", L".6.0.tmp" })
        makeVersion(versions, v);
    for (const wchar_t *v : { L"5.0", L"6.0", L"7.0" }) {
        const std::wstring path = join(staging, v);
        expect(CreateDirectoryW(path.c_str(), nullptr), "create staging directory");
        put(join(path, L"package.msi"), "fixture");
        if (wcscmp(v, L"7.0") != 0)
            makeOld(join(path, L"package.msi"));
        makeOld(path);
    }
    put(join(root, L"current.version"), "4.0\n");
    put(join(root, L"previous.version"), "3.0\n");
    put(join(staging, L"prepared.json"), R"({"schema":1,"version":"5.0"})");

    // Verify that only the unrelated old version is removed.
    {
        AnyKeepWin::MaintenanceLock lock(root);
        expect(lock.acquire(0), "maintenance lock acquired");
        AnyKeepWin::cleanupInstalledVersions(root, L"1.0");
    }
    expect(exists(join(versions, L"1.0")), "MSI-owned version kept");
    expect(!exists(join(versions, L"2.0")), "obsolete version removed");
    expect(exists(join(versions, L"3.0")), "rollback version kept");
    expect(exists(join(versions, L"4.0")), "active version kept");
    expect(exists(join(versions, L"5.0")), "prepared version kept");
    expect(!exists(join(versions, L".6.0.tmp")), "stale temporary version removed");
    expect(!exists(join(staging, L"6.0")), "stale staging directory removed");
    expect(exists(join(staging, L"5.0")), "prepared staging directory kept");
    expect(exists(join(staging, L"7.0")), "recent download in old staging directory kept");

    // Malformed prepared state must suppress destructive cleanup.
    makeVersion(versions, L"2.0");
    put(join(staging, L"prepared.json"), "{broken JSON");
    AnyKeepWin::cleanupInstalledVersions(root, L"1.0");
    expect(exists(join(versions, L"2.0")), "invalid prepared state protects all versions");

    // An invalid current pointer must also suppress cleanup.
    DeleteFileW(join(staging, L"prepared.json").c_str());
    put(join(root, L"current.version"), "../escape\n");
    AnyKeepWin::cleanupInstalledVersions(root, L"1.0");
    expect(exists(join(versions, L"2.0")), "invalid current pointer protects all versions");

    // Use Windows' temporary directory API for test fixtures and remove only
    // what the test itself created.
    for (const wchar_t *v : { L"1.0", L"2.0", L"3.0", L"4.0", L"5.0", L".6.0.tmp" }) {
        const std::wstring dir = join(versions, v);
        if (exists(dir)) {
            DeleteFileW(join(dir, L"anykeep.exe").c_str());
            RemoveDirectoryW(dir.c_str());
        }
    }
    RemoveDirectoryW(versions.c_str());
    DeleteFileW(join(staging, L"prepared.json").c_str());
    for (const wchar_t *v : { L"5.0", L"7.0" }) {
        DeleteFileW(join(join(staging, v), L"package.msi").c_str());
        RemoveDirectoryW(join(staging, v).c_str());
    }
    RemoveDirectoryW(staging.c_str());
    DeleteFileW(join(root, L"current.version").c_str());
    DeleteFileW(join(root, L"previous.version").c_str());
    DeleteFileW(join(join(root, L"update-logs"), L"cleanup.log").c_str());
    RemoveDirectoryW(join(root, L"update-logs").c_str());
    RemoveDirectoryW(root.c_str());
}

} // namespace

int main()
{
    policyTests();
    integrationTest();
    std::puts("Windows version retention tests passed");
    return 0;
}

#endif
