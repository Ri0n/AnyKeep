# Windows self-update architecture

## Goals

The unpackaged Windows build should behave like Telegram-style desktop updates:

1. Check a small HTTPS manifest in the background.
2. Download and verify a ZIP package without interrupting the user.
3. Unpack the package into a new version directory while the current version keeps running.
4. Show a system notification and a green banner in the note manager only after the new version is ready.
5. Switch versions quickly after **Update and restart**.
6. Roll back automatically if the new executable exits before it confirms a healthy startup.

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
- checks the configured manifest shortly after startup and then every six hours;
- downloads to `staging/<version>/package.zip.part`;
- verifies package size and SHA-256;
- validates every ZIP entry against the destination path and extracts through Windows PowerShell / .NET;
- validates `anykeep.exe` and `AnyKeepUpdater.exe`;
- renames `versions/.<version>.tmp` to `versions/<version>`;
- records the prepared version in `staging/prepared.json`;
- restores a prepared update after an application restart;
- exposes the green **Update and restart** banner;
- launches the updater from the prepared version.

The default manifest is configured by `ANYKEEP_UPDATE_MANIFEST_URL`. Development builds can override it with the `ANYKEEP_UPDATE_MANIFEST_URL` environment variable. `ANYKEEP_UPDATE_ROOT` can point a development build at a test installation tree.

### `AnyKeepUpdater.exe`

A native Win32 helper shipped inside every version directory. It:

1. waits for the old AnyKeep PID to exit;
2. writes `previous.version`;
3. atomically replaces `current.version` using a write-through temporary file and `MoveFileEx`;
4. starts the new `anykeep.exe` directly with a one-time startup marker;
5. keeps the new version if it confirms a healthy startup;
6. switches back and starts the previous version if the new process exits before confirmation;
7. restarts through the stable launcher if switching or startup fails.

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
start new AnyKeep and confirm startup
```

The slow work is finished before the banner appears. The button path only checkpoints notes, closes the old process, replaces a tiny pointer file, and starts the prepared application.

## Manifest schema

```json
{
  "schema": 1,
  "version": "4.0.1",
  "minimumLauncherProtocol": 1,
  "releaseNotesUrl": "https://anykeep.net/releases/4.0.1",
  "package": {
    "format": "zip",
    "url": "https://anykeep.net/updates/stable/AnyKeep-4.0.1-windows-x86_64.zip",
    "size": 12345678,
    "sha256": "..."
  }
}
```

The CMake target `windows_update_package` creates a clean install tree, runs `windeployqt`, and writes both the ZIP and `windows-x86_64.json` under `build/updates/stable`.

## Initial installation and uninstall

WiX remains responsible for the first installation, Start menu registration, AppUserModelID, Visual C++ runtime bootstrap, and Installed Apps entry. The MSI is per-user and installs into `%LOCALAPPDATA%\Programs\AnyKeep`.

The WiX package owns the stable launcher and the initial version directory. `RemoveFolderEx` removes updater-created versions, pointer files, staging data, and update logs during uninstall.

Changing an already released per-machine MSI to a per-user MSI is a separate migration problem: Windows Installer does not treat products in different installation contexts as a normal in-place major upgrade. If an old Program Files build has been distributed, the 4.0 installer should detect it and ask for a one-time uninstall or perform an explicit migration.

## Microsoft Store path

A later MSIX / Microsoft Store package can reuse the application binaries and UI but should not use the folder switcher. A process with package identity is detected through `GetCurrentPackageFamilyName`; the self-updater remains disabled and Windows / Microsoft Store owns update delivery.

The green banner may later be reused for Store-managed release notes, but it must not download or replace Store package files itself.

## Security before public rollout

The implemented first stage verifies HTTPS, expected archive size, and SHA-256 from the manifest. Before publishing an update manifest for real users, add a detached signature over a canonical manifest and pin the release public key in AnyKeep. SHA-256 alone protects against a damaged package but does not protect against compromise of both the manifest and package host.

Code signing of `anykeep.exe`, `AnyKeepUpdater.exe`, and `AnyKeepLauncher.exe` should also be part of the release pipeline. Archive extraction rejects entries that resolve outside the temporary version directory. It must still only be enabled for manifests trusted by the release-signature policy.

## Retention and cleanup still to add

The first stage keeps the previous version for rollback and does not aggressively remove older prepared versions. Before release, add a conservative cleanup pass that:

- never removes `current.version` or `previous.version` targets;
- keeps at least one known-good previous version;
- removes stale `.tmp` directories and completed staging packages;
- limits total retained update storage.
