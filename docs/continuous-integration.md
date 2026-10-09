# Continuous integration

AnyKeep uses GitHub Actions for reproducible desktop builds and package assembly.
The ordinary CI workflow does not publish releases or sign artifacts.

## Pull requests and pushes

`.github/workflows/ci.yml` validates the desktop application and the Android package path:

- Windows Server 2022 with MSVC, Ninja, and the complete CTest suite;
- Ubuntu 24.04 and 26.04 with GCC/Ninja and CTest;
- Intel macOS 15 as a compile check;
- Android arm64-v8a and x86_64 as real APK builds using the same setup as packaging.

Qt is installed from the current 6.11 series. Windows CI and Windows packaging
share `.github/actions/setup-windows-desktop`, which installs Qt, prepares the
MSVC environment, installs Conan, downloads the prebuilt QCA/Iris SDKs, and
sets the QCA runtime/plugin paths. Windows builds use Ninja with
`--parallel 4`.

Android CI and Android packaging share `.github/actions/build-android-apk`.
The action owns NDK/Qt setup, release SDK downloads, Android OpenSSL provisioning,
CMake configuration, and APK assembly. Package jobs only add artifact upload on
top, so CI exercises the same final-link path used for releases.

The checkout fetches full Git history because `AnyKeepMacro.cmake` derives the
application version from Git tags and the distance from the last tag.

## Package workflow

`.github/workflows/packages.yml` is the canonical package workflow.

The package channel is selected once by the planning job and reused by every
platform:

- a commit carrying a semantic version tag (`vX.Y.Z` or `X.Y.Z`) is always a `stable` build;
- a manual run may explicitly select `stable` or `nightly` and defaults to
  `nightly`;
- every other automatic package build uses the `nightly` channel.

Both stable and nightly builds use the same package matrix: Ubuntu 24.04 and
26.04 Debian packages, Windows x64, macOS arm64/x86_64, and Android
arm64-v8a/x86_64. A nightly GitHub release is updated only after every matrix
job succeeds. The rolling release therefore never contains a new Windows build
paired with stale or failed packages from another platform.

Stable builds use the exact dependency versions in `dependencies.lock.json`.
Nightly builds may advance Iris and QCA automatically within the
source-controlled `nightly_compatibility` line. The current policy is
`same-minor`: for example, an Iris 1.1.x pin may advance to a newer complete
1.1.x release, but not to 1.2.x. A dependency release is eligible only after it
publishes the full asset set required by the AnyKeep package matrix.
QtKeychain is resolved from AnyKeep's own packaged dependency releases using
the same conservative same-minor rule; a newer bundle is eligible only when
the complete Windows/macOS/Android asset set exists.

The 03:00 UTC scheduled run is skipped when both the AnyKeep commit and the
resolved compatible dependency set are identical to the markers recorded by
the last successful `nightly` release. Manual runs and version-tag builds are
never suppressed by this gate.

The separate `windows-nightly.yml` workflow is intentionally no longer needed.
Keeping nightly and stable assembly in one workflow prevents CMake flags,
dependency setup, update-channel selection, and package targets from drifting
apart.

## Windows distribution artifacts

The Windows package job keeps three distribution paths separate while deriving
them from one Release build tree:

- `AnyKeep.msi` is the canonical Windows Installer package;
- `AnyKeep.Installer-<version>.exe` is the interactive Burn bootstrapper and
  bootstraps the required Visual C++ Redistributable;
- `AnyKeep-<version>-windows-x86_64.msix` is the Microsoft Store package.

The same MSI is also the direct self-update payload. The job builds
`windows_update_package`, which performs a Windows Installer administrative
extraction to verify the version-owned runtime and then writes the selected
update channel under `build/package/updates/<channel>/`:

```text
AnyKeep-<version>-windows-x86_64.msi
AnyKeep-<version>-windows-x86_64.json
windows-x86_64.json
SHA256SUMS.txt
```

This makes the installer MSI and the self-update MSI the same bytes rather than
two independently assembled packages.

The Store identity is passed explicitly at CMake configure time from these
repository variables:

- `ANYKEEP_MSIX_IDENTITY_NAME` (`Package/Identity/Name`);
- `ANYKEEP_MSIX_PUBLISHER` (`Package/Identity/Publisher`);
- `ANYKEEP_MSIX_PUBLISHER_DISPLAY_NAME`
  (`Package/Properties/PublisherDisplayName`).

If any of them is absent, the MSIX target is skipped while MSI/Burn/update
artifacts are still built. Microsoft Store re-signs the submitted MSIX after
certification; direct-download MSI/EXE signing remains a separate publishing
step.

## Local Windows equivalent

A matching fast Release build uses the MSVC environment with Ninja:

```powershell
cmake -S . -B build/windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DANYKEEP_UPDATE_CHANNEL=nightly `
  -DANYKEEP_MSIX_IDENTITY_NAME="<Partner Center Identity Name>" `
  -DANYKEEP_MSIX_PUBLISHER="<Partner Center Publisher>" `
  -DANYKEEP_MSIX_PUBLISHER_DISPLAY_NAME="<Partner Center PublisherDisplayName>" `
  -DBUILD_TESTING=ON

cmake --build build/windows --parallel 4
ctest --test-dir build/windows --output-on-failure
cmake --build build/windows --target windows_update_package burn_installer --parallel 4
cmake --build build/windows --target msix_package --parallel 4
```

Qt, Conan, QCA/Iris SDKs, WiX, and the MSVC environment must be available in the
same way as in CI.