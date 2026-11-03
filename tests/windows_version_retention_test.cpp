#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../src/win/versioncleanup.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

void expect(bool result, const char *message)
{
    if (!result) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

void put(const fs::path &path, const std::string &data)
{
    std::ofstream file(path, std::ios::binary);
    file << data;
    expect(file.good(), "write fixture");
}

void version(const fs::path &root, const wchar_t *name, bool old = true)
{
    const fs::path directory = root / L"versions" / name;
    expect(fs::create_directory(directory), "create version");
    put(directory / L"anykeep.exe", "fixture");
    if (old)
        fs::last_write_time(directory, fs::file_time_type::clock::now() - std::chrono::hours(48));
}

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path()
        / (L"anykeep-retention-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root / L"versions");
    fs::create_directories(root / L"staging");

    version(root, L"1.0"); // MSI-owned
    version(root, L"2.0"); // obsolete
    version(root, L"3.0"); // previous
    version(root, L"4.0"); // current
    version(root, L"5.0", false); // too new for cleanup
    version(root, L".6.0.tmp"); // never treat a temporary update as a version

    put(root / L"current.version", "4.0\n");
    put(root / L"previous.version", "3.0\n");
    put(root / L"staging" / L"prepared.json", R"({"version":"7.0"})");
    AnyKeepWin::cleanupInstalledVersions(root.wstring(), L"1.0");
    expect(fs::exists(root / L"versions" / L"2.0"), "prepared update must block cleanup");

    fs::remove(root / L"staging" / L"prepared.json");
    AnyKeepWin::cleanupInstalledVersions(root.wstring(), L"1.0");
    expect(!fs::exists(root / L"versions" / L"2.0"), "obsolete version removed");
    expect(fs::exists(root / L"versions" / L"1.0"), "MSI-owned version retained");
    expect(fs::exists(root / L"versions" / L"3.0"), "previous version retained");
    expect(fs::exists(root / L"versions" / L"4.0"), "current version retained");
    expect(fs::exists(root / L"versions" / L"5.0"), "recent version retained");
    expect(fs::exists(root / L"versions" / L".6.0.tmp"), "temporary update retained");

    version(root, L"2.0");
    put(root / L"current.version", "../invalid\n");
    AnyKeepWin::cleanupInstalledVersions(root.wstring(), L"1.0");
    expect(fs::exists(root / L"versions" / L"2.0"), "invalid pointer must block cleanup");

    fs::remove_all(root);
    std::puts("Windows version retention tests passed");
    return 0;
}

#endif
