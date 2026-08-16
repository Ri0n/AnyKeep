# Windows self-update architecture

## Goals

The unpackaged Windows build should behave like Telegram-style desktop updates:

1. Check a small HTTPS manifest in the background.
2. Download and verify the release MSI without interrupting the user.
3. Use Windows Installer administrative extraction to prepare the MSI's version directory while the current version keeps running.
4. Show a system notification and a green banner in the note manager only after the new version is ready.
5. Switch versions quickly after **Update and restart**.
6. Confirm the new version after it remains alive for 60 seconds, or when it exits normally before that timer elapses.
7. Roll back automatically if the new executable cannot start, exits abnormally, or never reaches the startup probe.

Microsoft Store / MSIX builds reuse the same update UI and scheduling policy, but not the MSI staging/switcher path. `UpdateController` detects package identity and delegates discovery, download, and installation to `Windows.Services.Store.StoreContext`.

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

Owned by `AnyKeep::Main` and exposed to the notes-manager QML window. It keeps one update UI while selecting the delivery backend from process package identity. It:

- exposes an automatic-check checkbox and an independent **Check for updates** button in Options for both Windows distribution modes;
- for unpackaged MSI installs, checks the configured HTTPS manifest shortly after startup and then every six hours when automatic checks are enabled;
- downloads the direct-update MSI to `staging/<version>/package.msi.part` and verifies package size and SHA-256;
- runs `msiexec /a ... /qn TARGETDIR=<staging admin image>` to create an administrative image without installing the product;
- locates and validates the MSI-owned `versions/<version>` payload containing `anykeep.exe` and `AnyKeepUpdater.exe`;
- renames `versions/.<version>.tmp` to `versions/<version>`, records the prepared version in `staging/prepared.json`, restores it after an application restart, and launches the prepared updater;
- for package-identity / Microsoft Store builds, bypasses all MSI staging and uses `Windows.Services.Store.StoreContext` to discover, download, and install Store package updates;
- exposes the same green update banner for both backends.

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
background MSI download
    ↓
size + SHA-256 verification
    ↓
msiexec /a into a staging administrative image
    ↓
move its versions/<version> payload to versions/.<version>.tmp
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
  "schema": 2,
  "version": "4.0.1",
  "minimumLauncherProtocol": 1,
  "package": {
    "format": "msi",
    "url": "AnyKeep-4.0.1-windows-x86_64.msi",
    "size": 12345678,
    "sha256": "..."
  }
}
```

### Generating update artifacts

`windows_update_package` remains the single Windows update-publishing entry point for local Visual Studio builds and CI, but the update payload is now the **same MSI used for installation**. There is no separate update ZIP.

The target first builds the normal WiX MSI from `windows_runtime_install`. It then performs a quiet Windows Installer administrative extraction (`msiexec /a`) into `build/update-work/<channel>/admin-image` and verifies that the image contains exactly the expected version-owned runtime (`versions/<version>/anykeep.exe` and `AnyKeepUpdater.exe`). Only after that check succeeds does it copy the MSI into the update channel directory and calculate the final manifest/hash.

Configure a stable Visual Studio build in the usual way and build:

```powershell
cmake --build build --config Release --target windows_update_package
```

The default output is:

```text
build/updates/stable/
├── AnyKeep-<version>-windows-x86_64.msi
├── AnyKeep-<version>-windows-x86_64.json
├── windows-x86_64.json
└── SHA256SUMS.txt
```

The MSI owns both the stable launcher and `versions/<version>` runtime. Initial installation uses the MSI normally (through Burn when the Visual C++ redistributable is needed); self-update downloads the same MSI and uses only its administrative-image `versions/<version>` payload. Thus one Windows release artifact is the canonical source for both installation and self-update.

For a nightly build, configure a separate build tree with:

```powershell
cmake -S . -B build-nightly -G "Visual Studio 17 2022" -A x64 `
    -DANYKEEP_UPDATE_CHANNEL=nightly
cmake --build build-nightly --config Release --target windows_update_package
```

That build checks `https://anykeep.net/updates/nightly/windows-x86_64.json` and writes artifacts under `build-nightly/updates/nightly`. `ANYKEEP_UPDATE_SERVER_ROOT` changes the common server root. `ANYKEEP_UPDATE_MANIFEST_URL` may override the compiled manifest URL explicitly.

Package URLs in generated manifests are relative by default, so the whole channel directory can be served locally or uploaded unchanged. `ANYKEEP_UPDATE_BASE_URL` remains available if manifests and packages later need different hosts.

`ANYKEEP_UPDATE_OUTPUT_DIR` can redirect artifacts to a caller-selected directory:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
    -DANYKEEP_UPDATE_CHANNEL=nightly `
    -DANYKEEP_UPDATE_OUTPUT_DIR="$env:RUNNER_TEMP\anykeep-update"
cmake --build build --config Release --target windows_update_package
```

For signed direct-download releases, sign the MSI first and then rerun `wix/make-update-package.cmake.in` against those final bytes. The script deliberately hashes and publishes the supplied MSI bytes, so the manifest never describes a pre-signing artifact.

Publishing order still matters: upload the versioned MSI and immutable version manifest first, then replace `windows-x86_64.json` last. The latest manifest is the only mutable channel pointer.

## Initial installation and uninstall

WiX remains responsible for the first installation, Start menu registration, AppUserModelID, Visual C++ runtime bootstrap, and Installed Apps entry. The MSI is per-user and installs into `%LOCALAPPDATA%\Programs\AnyKeep`.

The MSI may also be downloaded and launched directly, without the Burn bootstrapper. For a fresh direct installation it checks both the x64 Visual C++ v14 Redistributable registration and the installed `vcruntime140.dll` version. The minimum accepted version is derived from the MSVC toolset used for that build (or an explicit `ANYKEEP_REQUIRED_VC_RUNTIME_VERSION` override), so the prerequisite floor does not silently become stale when the CI image updates Visual Studio. Administrative extraction (`msiexec /a`), repair, and uninstall bypass that prerequisite check. Burn remains responsible for installing the prerequisite automatically before it invokes the MSI.

Burn does not pin Microsoft's mutable `aka.ms` permalink directly. When `burn_installer` is built, CMake downloads the current x64 v14 Redistributable through `https://aka.ms/vc14/vc_redist.x64.exe`, records the final `https://download.visualstudio.microsoft.com/...` URL, and runs `wix burn remotepayload` on those exact bytes. WiX supplies the SHA-512, size, version and product metadata embedded in the bundle. An old AnyKeep bootstrapper therefore keeps requesting the exact Microsoft payload it was built and hashed against even after the rolling permalink advances. The final redirect host is allow-listed; a Microsoft CDN topology change intentionally breaks the build until it is reviewed.
The actual post-package logic lives in `wix/make-burn-installer.cmake`. It takes an MSI path explicitly and has no Qt build-tree dependency, so a future signing/publishing workflow can build Burn around the exact MSI bytes it intends to ship. The normal `burn_installer` target is only a convenience wrapper around that script.

The WiX package owns the stable launcher and the initial version directory. `RemoveFolderEx` removes updater-created versions, pointer files, staging data, and update logs during uninstall.

Changing an already released per-machine installer to a per-user MSI is a separate migration problem: Windows Installer does not treat products in different installation contexts as a normal in-place major upgrade. The per-user MSI checks the 32-bit and 64-bit HKLM uninstall entries used by previous AnyKeep and QtNote installers. If one is present, installation stops with instructions to uninstall the old Program Files build manually; user notes and settings are not removed.

## Microsoft Store path

The `msix_package` target packages the same deployed Qt application tree for Microsoft Store without the MSI-specific launcher/update helpers. `anykeep.exe` is the MSIX application entry point, and the manifest declares a full-trust packaged desktop application plus the Store-managed `Microsoft.VCLibs.140.00.UWPDesktop` dependency. The Store identity (`Name`, `Publisher`, and `PublisherDisplayName`) is supplied from Partner Center rather than invented in the source tree.

A process with package identity is detected through `GetCurrentPackageFamilyName`. In that mode the direct MSI updater remains disabled, while `UpdateController` uses `StoreContext::GetAppAndOptionalStorePackageUpdatesAsync()` for the existing **Check for updates** UI. Store package bytes are never downloaded or replaced directly by AnyKeep.

For automatic checks, AnyKeep first asks the Store whether silent package delivery is currently allowed. When it is, the update is downloaded with `TrySilentDownloadStorePackageUpdatesAsync()`. After download, the application decides whether it is a safe time to restart: if no visible note editor is open, all editor state is synchronously checkpointed and the Store installation starts immediately; if a note is visible, AnyKeep leaves it alone and shows the normal update notification/banner so the user chooses when to restart. This mirrors Microsoft's Store update sample, which deliberately leaves the "good time to restart" decision to the application.

A user-triggered Store update uses the Store request APIs when silent delivery is unavailable, so Windows owns the consent dialogs. `StoreContext` is associated with an AnyKeep HWND through `IInitializeWithWindow` before invoking methods that can display those dialogs. Immediately before installation, AnyKeep checkpoints every open editor.

Store builds register the process with `RegisterApplicationRestart()` as soon as the Store backend is created, using flags that disable crash, hang, and reboot relaunches while retaining restart after an application update. The registration is refreshed immediately before an AnyKeep-initiated installation and intentionally remains registered until process termination. This also covers Store-driven automatic updates that happen outside AnyKeep's own check cycle: Windows can close the old packaged process for deployment and relaunch the new version afterwards. Restart Manager only restarts a process that has been running for at least 60 seconds, so the first automatic Store check is delayed to 65 seconds instead of the unpackaged updater's 20-second startup delay. This restart path must still be validated with an actual Store flight because Store package discovery and installation require a Store-associated package identity.

The packaged process also keeps the manifest-provided AUMID instead of overriding it with the unpackaged `com.github.ri0n.AnyKeep` AppUserModelID.

## Security before public rollout

The implemented first stage verifies HTTPS, expected MSI size, and SHA-256 from the manifest. Manifest signatures and Authenticode signing are intentionally deferred until the unsigned update path is stable. Before public rollout, add a detached signature over a canonical manifest with a rotation-capable trust policy, and code-sign the installer, launcher, updater, application, and project-owned executable modules. SHA-256 alone protects against a damaged package but does not protect against compromise of both the manifest and package host.

MSI extraction is delegated to Windows Installer administrative mode, and AnyKeep accepts only the expected `versions/<version>` payload. Public update delivery must remain disabled until the release-signature policy is implemented. A client that is too old to understand a future trust-key rotation may direct the user to download a fresh installer manually.

## Retention and cleanup still to add

The first stage keeps the previous version for rollback and does not aggressively remove older prepared versions. Before release, add a conservative cleanup pass that:

- never removes `current.version` or `previous.version` targets;
- keeps at least one known-good previous version;
- removes stale `.tmp` directories and completed staging packages;
- limits total retained update storage.
