# Continuous integration

AnyKeep uses GitHub Actions for reproducible desktop builds and package assembly.
The ordinary CI workflow does not publish releases or sign artifacts.

## Pull requests and pushes

`.github/workflows/ci.yml` validates the desktop application and the Android package path:

- Windows Server 2022 with MSVC, Ninja, and the complete CTest suite;
- Ubuntu 24.04 and 26.04 with GCC/Ninja and CTest;
- Debian 13 as a real containerized `.deb` package build using the release packaging path;
- Intel macOS 15 as a compile check;
- Android arm64-v8a and x86_64 as real APK builds using the same setup as packaging.

Qt is installed from the current 6.11 series. Windows CI and Windows packaging
share `.github/actions/setup-windows-desktop`, which installs Qt, prepares the
MSVC environment, installs Conan, downloads the prebuilt QCA/Iris SDKs, and
sets the QCA runtime/plugin paths. Windows builds use Ninja with
`--parallel 4`.

Ubuntu and Debian package jobs share `.github/actions/build-deb-package`,
which in turn uses the same distribution-aware QCA/Iris Debian-package installer.
The Debian 13 job runs inside `debian:13`, so it validates the actual Trixie
package dependency set rather than approximating Debian on an Ubuntu runner.

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
26.04 Debian packages, a native Debian 13 package build, Windows x64, macOS
arm64/x86_64, and Android arm64-v8a/x86_64. The selected GitHub Release is
updated only after every matrix job succeeds. Nightly uses the rolling
`nightly` release; a semantic-version-tag build publishes to that version tag;
an explicit untagged stable build uses the rolling `stable` release.

Stable builds use the exact dependency versions in `dependencies.lock.json`.
Nightly builds may advance Iris and QCA automatically within the
source-controlled `nightly_compatibility` line. The current policy is
`same-minor`: for example, an Iris 1.1.x pin may advance to a newer complete
1.1.x release, but not to 1.2.x. A dependency release is eligible only after it
publishes the full asset set required by the AnyKeep package matrix.
QtKeychain is resolved from AnyKeep's own packaged dependency releases using
the same conservative same-minor rule; a newer bundle is eligible only when
the complete Windows/macOS/Android asset set exists.

The 03:00 UTC scheduled run is skipped when no build-relevant repository files
have changed since the last successful `nightly` release and the resolved
compatible dependency set is unchanged. Documentation-only changes under
`docs/`, README/AGENTS files, and license text do not wake the nightly build;
source, CMake, packaging, workflow/action, and other build inputs do. Manual
runs and semantic-version-tag builds are never suppressed by this gate.

The separate `windows-nightly.yml` workflow is intentionally no longer needed.
Keeping nightly and stable assembly in one workflow prevents CMake flags,
dependency setup, update-channel selection, and package targets from drifting
apart.


The post-package `Publish AnyKeep updates` workflow derives the updater channel
from the Windows artifact itself. The package workflow first uploads all binary
artifacts to GitHub Releases. The follow-up workflow validates the versioned MSI
against its manifest and then publishes only `windows-x86_64.json` to
`anykeep.net/updates/<channel>/`. Stable and nightly therefore share the same
control-plane manifest service without duplicating binary storage or bandwidth.

## Windows distribution artifacts

The Windows package job keeps three distribution paths separate while deriving
them from one Release build tree:

- `AnyKeep.msi` is the canonical build-tree Windows Installer output; the GitHub Release publishes the identical bytes under the versioned updater filename instead of duplicating both names;
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

## QCA and Iris minimum supported runtime versions

The minimum supported runtime versions are declared centrally in
`dependencies.lock.json` under `qca.minimum_version` and
`iris.minimum_version`. The separate `tag` and `commit` fields select
the reproducible stable build; nightly packaging may choose a newer
compatible published release. Build tags must never fall below the
declared minimum.

The `packaging/debian/runtime-depends.py` helper validates the lock
and creates a single `${runtime:Depends}` substitution with explicit
minimums for QCA's core library, QCA's crypto plugins and the Iris
runtime library. Each QCA3/Iris Debian profile references the
substitution, and `debian/rules` supplies it to `dh_gencontrol`.
No Debian profile hardcodes a dependency minimum. The distro-native
`qt6-noble` profile does not depend on these separately packaged
QCA3 and Iris runtimes.

`.github/scripts/resolve-package-dependencies.py` rejects a stable
build tag below its component's minimum and includes the minimums in
its dependency fingerprint. CI unit tests verify distinct pinned
build versions versus minimums, malformed versions and changed
lock values; Debian packaging CI inspects the actual generated
`libanykeep3` package's `Depends` field for all three requirements.

A shared-library ABI/SONAME baseline and a consumer project's
minimum supported version are different concepts. The lock records
the latter and may be raised for fixes or features even when the ABI
is unchanged. Existing Debian artifacts must be rebuilt for changes
to their package dependencies to take effect.
