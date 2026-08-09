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

## Unsigned Windows nightly

`.github/workflows/windows-nightly.yml` runs every day at 03:00 UTC and can also
be started manually from the Actions tab. It uses the same Visual Studio 2022
Release configuration as the Windows CI job, runs CTest, then builds both:

```text
windows_update_package
burn_installer
```

The workflow uploads two GitHub Actions artifacts:

- the `nightly` update directory containing the versioned MSI, immutable version manifest,
  channel manifest, and SHA256SUMS;
- the MSI and Burn installer executable.

These artifacts are intentionally unsigned and are not copied to
`anykeep.net`. A later signing/publishing workflow should deep-sign the MSI (including AnyKeep-owned PE files), rerun the update-manifest script against the signed MSI, build/sign the Burn bootstrapper, and only then publish the final channel artifacts.

## Local equivalent

The Windows CI configuration intentionally uses an ordinary Visual Studio
multi-config build. The essential commands are:

```powershell
cmake -S . -B build/windows `
  -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_CONFIGURATION_TYPES=Release `
  -DANYKEEP_UPDATE_CHANNEL=nightly `
  -DBUILD_TESTING=ON

cmake --build build/windows --config Release --parallel 4
ctest --test-dir build/windows -C Release --output-on-failure
cmake --build build/windows --config Release `
  --target windows_update_package burn_installer --parallel 4
```

Qt, Conan, and WiX still need to be available exactly as they do for a manual
release build.
