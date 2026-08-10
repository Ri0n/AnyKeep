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
Release configuration as the Windows CI job and runs CTest. The persistent
artifact is deliberately only the canonical unsigned `AnyKeep.msi`, which is
the future SignPath input. Update manifests and release installers must not be
published from bytes that will later be changed by Authenticode signing.

The workflow also builds an unsigned Burn bootstrapper as a smoke test but does
not upload it. That test follows Microsoft's rolling `vc14` redirect, pins the
resolved `download.visualstudio.microsoft.com` object and asks WiX to generate
the remote payload hash/size/version. It therefore catches redirect or WiX
authoring changes without creating another release-looking artifact.

The intended signed publishing pipeline is sequential: deep-sign the MSI, run
`windows_update_package` against those signed bytes, build Burn around the
signed MSI with `wix/make-burn-installer.cmake`, complete WiX's bundle-signing
flow, and only then upload public artifacts. The Burn script accepts an
external MSI and therefore works in a fresh post-signing Windows job without
the original Qt build tree. SignPath Foundation requires manual approval for
each signing request, so the actual signing workflow is kept separate until
the SignPath project, policies and artifact configurations exist.

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
cmake --build build/windows --config Release --target package --parallel 4
cmake --build build/windows --config Release --target burn_installer --parallel 4
```

Qt, Conan, and WiX still need to be available exactly as they do for a manual
release build.
