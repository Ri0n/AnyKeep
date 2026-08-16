# Continuous integration

AnyKeep uses GitHub Actions for reproducible desktop builds. The initial CI
layer deliberately does not publish releases and does not sign artifacts; code
signing is intended to be added after the unsigned pipeline is stable.

## Pull requests and pushes

`.github/workflows/ci.yml` builds the desktop application on three platforms:

- Windows 2025 with the Visual Studio 2022 x64 generator and Release only;
- Ubuntu 24.04 with GCC/Ninja, followed by the complete CTest suite;
- Intel macOS 15 as a compile check for the application and macOS integration.

Qt is pinned to the same desktop version used by the project (`6.10.2`). The
Windows build uses the project's existing Conan dependency path. Linux and
macOS use bundled QCA and QtKeychain while leaving the optional XMPP stack off
for the first cross-platform CI pass. Windows keeps its normal bundled XMPP
stack enabled, so the priority platform exercises the same dependency layout as
local release builds.

The checkout fetches full Git history because `AnyKeepMacro.cmake` derives the
application version from Git tags and the distance from the last tag.

## Windows nightly packages

`.github/workflows/windows-nightly.yml` runs every day at 03:00 UTC and can also
be started manually from the Actions tab. It uses the same Visual Studio 2022
Release configuration as the Windows CI job and runs CTest.

The nightly keeps three Windows distribution paths separate:

- `AnyKeep.msi` is the canonical Windows Installer package and remains the
  payload used by AnyKeep's own restart-to-update path outside the Store.
- `AnyKeep.Installer-<version>.exe` is the interactive Burn bootstrapper. It
  resolves Microsoft's current x64 VC++ v14 Redistributable to a concrete CDN
  object, pins that payload with WiX-generated metadata, and then installs the
  MSI.
- `AnyKeep-<version>-windows-x86_64.msix` is the Microsoft Store package. It
  starts `anykeep.exe` directly, excludes `AnyKeepLauncher.exe` and
  `AnyKeepUpdater.exe`, declares `runFullTrust`, and depends on the Store-managed
  `Microsoft.VCLibs.140.00.UWPDesktop` framework package. Its retail framework identity is discovered from the installed Windows Extension SDK when available. AnyKeep detects package
  identity at runtime and routes the existing update UI through Microsoft Store APIs instead of the direct MSI updater.

The Store assigns the package identity after the product name is reserved in
Partner Center. Until then, the MSIX step is skipped. Configure these GitHub
repository variables with the exact case-sensitive values from Partner Center:

- `ANYKEEP_MSIX_IDENTITY_NAME` (`Package/Identity/Name`);
- `ANYKEEP_MSIX_PUBLISHER` (`Package/Identity/Publisher`);
- `ANYKEEP_MSIX_PUBLISHER_DISPLAY_NAME`
  (`Package/Properties/PublisherDisplayName`).

Once all three are present, nightly artifacts contain MSI, EXE and MSIX. The
MSIX produced by CI is intentionally not CA-signed: Microsoft Store re-signs an
MSIX after certification. Direct-download MSI/EXE files are a separate signing
problem; Microsoft Store does not sign them for distribution outside the Store.

`msix_package` uses `MakeAppx.exe` from the installed Windows SDK. The package
minimum is Windows 10 version 1809, matching Qt 6.11's supported Windows floor.
The MSIX is for Store submission; sideloading the CI artifact requires a
separate signing/dependency setup and is not the release path.

## Local equivalent

The Windows CI configuration intentionally uses an ordinary Visual Studio
multi-config build. The essential commands are:

```powershell
cmake -S . -B build/windows `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_CONFIGURATION_TYPES=Release `
  -DANYKEEP_UPDATE_CHANNEL=nightly `
  -DANYKEEP_MSIX_IDENTITY_NAME="<Partner Center Identity Name>" `
  -DANYKEEP_MSIX_PUBLISHER="<Partner Center Publisher>" `
  -DANYKEEP_MSIX_PUBLISHER_DISPLAY_NAME="<Partner Center PublisherDisplayName>" `
  -DBUILD_TESTING=ON

cmake --build build/windows --config Release --parallel 4
ctest --test-dir build/windows -C Release --output-on-failure
cmake --build build/windows --config Release --target package --parallel 4
cmake --build build/windows --config Release --target burn_installer --parallel 4
cmake --build build/windows --config Release --target msix_package --parallel 4
```

Qt, Conan, and WiX still need to be available exactly as they do for a manual
release build.
