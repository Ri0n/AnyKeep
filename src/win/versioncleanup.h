#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace AnyKeepWin {

// Shared by the launcher cleanup and the updater's pointer-switch/rollback
// window. A skipped cleanup must never prevent the application from starting.
class MaintenanceLock {
public:
    explicit MaintenanceLock(const std::wstring &root);
    ~MaintenanceLock();

    MaintenanceLock(const MaintenanceLock &) = delete;
    MaintenanceLock &operator=(const MaintenanceLock &) = delete;

    bool acquire(DWORD timeoutMs);
    void release();

private:
    HANDLE mutex_ = nullptr;
    bool   locked_ = false;
};

// Only the installed launcher knows the MSI-owned base version. Run cleanup
// there, never from an arbitrary versioned application or MSIX executable.
void cleanupInstalledVersions(const std::wstring &root, const std::wstring &msiVersion);
void spawnCleanupHelper(const std::wstring &root);

} // namespace AnyKeepWin

#endif
