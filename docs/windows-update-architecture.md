# Windows self-update architecture

## Goals

The unpackaged Windows build should behave like Telegram-style desktop updates:

1. Check a small HTTPS manifest in the background.
2. Download and verify a ZIP package without interrupting the user.
3. Unpack the package into a new version directory while the current version keeps running.
4. Show a system notification and a green banner in the note manager only after the new version is ready.
5. Switch versions quickly after **Update and restart**.
6. Confirm the new version after it remains alive for 60 seconds, or when it exits normally before that timer elapses.
7. Roll back automatically if the new executable cannot start, exits abnormally, or never reaches the startup probe.

Microsoft Store / MSIX builds do not use this updater. `UpdateController` detects package identity and reports that updates are store-managed.

## Installed layout

```text
%LOCALAPPDATA%\Programs\AnyKeep\
├── AnyKeepLauncher.exe
├── current.version
├── previous.version
├── versions\
│   ├── 4.0.0\
│   │   ├── anykeep.exe
│   │   ├── AnyKeepUpdater.exe
│   │   ├── anykeep.dll
│   │   ├── plugins\
│   │   └── ...
│   └── 4.0.1\
├── staging\
└── update-logs\
```

The Start menu shortcut and Windows autostart entry always point to `AnyKeepLauncher.exe`. The launcher reads `current.version` and starts `versions/<version>/anykeep.exe`.

`current.version` is deliberately not owned by MSI. On the first launch, the launcher falls back to the initial version compiled into it and creates the pointer atomically. This prevents MSI repair from resetting a self-updated installation to the original version.

## Components

### `AnyKeepLauncher.exe`

A very small native Win32 executable with no Qt dependency. It:

- validates `current.version`;
- falls back to `previous.version` when the current version is incomplete;
- falls back to the initial MSI-installed version on the first run;
- repairs a missing pointer atomically;
- forwards the original command line;
- starts the selected application version.

### `UpdateController`

Owned by `AnyKeep::Main` and exposed to the notes-manager QML window. It:

- disables itself for package-identity / Microsoft Store builds;
- checks the configured manifest shortly after startup and then every six hours when automatic checks are enabled;
- exposes an automatic-check checkbox and an independent **Check for updates** button in Options;
- downloads to `staging/<version>/package.zip.part`;
- verifies package size and SHA-256;
- validates every ZIP entry against the destination path and extracts through Windows PowerShell / .NET;
- validates `anykeep.exe` and `AnyKeepUpdater.exe`;
- renames `versions/.<version>.tmp` to `versions/<version>`;
- records the prepared version in `staging/prepared.json`;
- restores a prepared update after an application restart;
- exposes the green **Update and restart** banner;
- launches the updater from the prepared version.

The default manifest is derived from `ANYKEEP_UPDATE_SERVER_ROOT` and `ANYKEEP_UPDATE_CHANNEL` (stable by default); `ANYKEEP_UPDATE_MANIFEST_URL` is an explicit configure-time override. In an `ANYKEEP_DEVEL` build, the updater is disabled unless the `ANYKEEP_UPDATE_ROOT` environment variable contains a test installation root. The development launcher honours the same variable, so it may be started from Qt Creator while exercising that test root. Development builds may also override the manifest at runtime with the `ANYKEEP_UPDATE_MANIFEST_URL` environment variable; it accepts HTTPS and, for local-network test servers, HTTP. Release builds ignore runtime environment overrides, accept HTTPS only, and derive the installation root from the versioned launcher layout.

### `AnyKeepUpdater.exe`

A native Win32 helper shipped inside every version directory. It:

1. waits for the old AnyKeep PID to exit;
2. writes `previous.version`;
3. atomically replaces `current.version` using a write-through temporary file and `MoveFileEx`;
4. starts the new `anykeep.exe` directly with a one-time startup marker;
5. waits for the application to create that marker after 60 seconds of healthy operation;
6. also accepts a normal application exit before the timer elapses;
7. terminates and rolls back a process that never reaches the startup probe within two minutes;
8. switches back and starts the previous version after an abnormal early exit;
9. restarts through the stable launcher if switching or startup fails.

A version may therefore ship newer switch and migration logic without first replacing a permanently installed updater. Future migration operations should remain narrow: Windows integration and launcher protocol changes belong here, while note/database migrations belong in AnyKeep with a backup and explicit compatibility policy.

## Update flow

```text
manifest check
    ↓
background ZIP download
    ↓
size + SHA-256 verification
    ↓
extract to versions/.<version>.tmp
    ↓
validate + rename to versions/<version>
    ↓
system notification + green manager banner
    ↓
checkpoint open notes
    ↓
start updater from the new version and quit
    ↓
atomic current.version switch
    ↓
start new AnyKeep and observe it for 60 seconds
```

The slow work is finished before the banner appears. The button path only checkpoints notes, closes the old process, replaces a tiny pointer file, and starts the prepared application.

## Manifest schema

```json
{
  "schema": 1,
  "version": "4.0.1",
  "minimumLauncherProtocol": 1,
  "package": {
    "format": "zip",
    "url": "AnyKeep-4.0.1-windows-x86_64.zip",
    "size": 12345678,
    "sha256": "..."
  }
}
```

### Generating update artifacts

`windows_update_package` is the single packaging entry point for both local Visual Studio builds and CI. It never stages through the normal installation prefix. Instead it creates an isolated install tree under `build/update-work/<channel>/install`, lets the normal CMake install rules run `windeployqt` there, and then creates the update artifacts.

Configure a stable Visual Studio build in the usual way and build:

```powershell
cmake --build build --config Release --target windows_update_package
```

The default output is:

```text
build/updates/stable/
├── AnyKeep-<version>-windows-x86_64.zip
├── AnyKeep-<version>-windows-x86_64.json
├── windows-x86_64.json
└── SHA256SUMS.txt
```

The ZIP contains one version directory payload: `anykeep.exe`, `AnyKeepUpdater.exe`, project DLLs/plugins, Qt runtime files and translations. It deliberately excludes the stable launcher and installation state (`AnyKeepLauncher.exe`, `current.version`, `previous.version`, `versions`, `staging`, and update logs).

For a nightly build, configure a separate build tree with:

```powershell
cmake -S . -B build-nightly -G "Visual Studio 17 2022" -A x64 `
    -DANYKEEP_UPDATE_CHANNEL=nightly
cmake --build build-nightly --config Release --target windows_update_package
```

That build is compiled to check `https://anykeep.net/updates/nightly/windows-x86_64.json` and writes artifacts under `build-nightly/updates/nightly`. `ANYKEEP_UPDATE_SERVER_ROOT` changes the common server root. `ANYKEEP_UPDATE_MANIFEST_URL` may override the compiled manifest URL explicitly.

Package URLs in generated manifests are relative by default, so the whole channel directory can be served locally or uploaded unchanged. For example, a development HTTP server may expose `updates/nightly/windows-x86_64.json` next to its ZIP. `ANYKEEP_UPDATE_BASE_URL` is an optional escape hatch when manifests and packages later need different hosts.

`ANYKEEP_UPDATE_OUTPUT_DIR` can redirect artifacts to a caller-selected directory. This is useful in GitHub Actions, where the same target can write directly to an artifact/publish directory:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DANYKEEP_UPDATE_CHANNEL=nightly `
    -DANYKEEP_UPDATE_OUTPUT_DIR="$env:RUNNER_TEMP\anykeep-update"
cmake --build build --config Release --target windows_update_package
```

Publishing order matters: upload the versioned ZIP and versioned manifest first, then replace `windows-x86_64.json` last. The latest manifest is the only mutable channel pointer. This prevents clients from observing a manifest before its referenced package is available.

## Initial installation and uninstall

WiX remains responsible for the first installation, Start menu registration, AppUserModelID, Visual C++ runtime bootstrap, and Installed Apps entry. The MSI is per-user and installs into `%LOCALAPPDATA%\Programs\AnyKeep`.

The WiX package owns the stable launcher and the initial version directory. `RemoveFolderEx` removes updater-created versions, pointer files, staging data, and update logs during uninstall.

Changing an already released per-machine installer to a per-user MSI is a separate migration problem: Windows Installer does not treat products in different installation contexts as a normal in-place major upgrade. The per-user MSI checks the 32-bit and 64-bit HKLM uninstall entries used by previous AnyKeep and QtNote installers. If one is present, installation stops with instructions to uninstall the old Program Files build manually; user notes and settings are not removed.

## Microsoft Store path

A later MSIX / Microsoft Store package can reuse the application binaries and UI but should not use the folder switcher. A process with package identity is detected through `GetCurrentPackageFamilyName`; the self-updater remains disabled and Windows / Microsoft Store owns update delivery.

The green banner may later be reused for Store-managed release notes, but it must not download or replace Store package files itself.

## Security before public rollout

The implemented first stage verifies HTTPS, expected archive size, and SHA-256 from the manifest. Manifest signatures and Authenticode signing are intentionally deferred until the unsigned update path is stable. Before public rollout, add a detached signature over a canonical manifest with a rotation-capable trust policy, and code-sign the installer, launcher, updater, application, and project-owned executable modules. SHA-256 alone protects against a damaged package but does not protect against compromise of both the manifest and package host.

Archive extraction rejects entries that resolve outside the temporary version directory. Public update delivery must remain disabled until the release-signature policy is implemented. A client that is too old to understand a future trust-key rotation may direct the user to download a fresh installer manually.

## Retention and cleanup still to add

The first stage keeps the previous version for rollback and does not aggressively remove older prepared versions. Before release, add a conservative cleanup pass that:

- never removes `current.version` or `previous.version` targets;
- keeps at least one known-good previous version;
- removes stale `.tmp` directories and completed staging packages;
- limits total retained update storage.
