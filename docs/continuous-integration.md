# Continuous integration

AnyKeep uses GitHub Actions for reproducible desktop builds and package assembly.
The ordinary CI workflow does not publish releases or sign artifacts.

## Pull requests and pushes

`.github/workflows/ci.yml` builds the desktop application on three platforms:

- Windows Server 2022 with MSVC, Ninja, and the complete CTest suite;
- Ubuntu 24.04 and 26.04 with GCC/Ninja and CTest;
- Intel macOS 15 as a compile check.

Qt is installed from the current 6.11 series. Windows CI and Windows packaging
share `.github/actions/setup-windows-desktop`, which installs Qt, prepares the
MSVC environment, installs Conan, downloads the prebuilt QCA/Iris SDKs, and
sets the QCA runtime/plugin paths. Windows builds use Ninja with
`--parallel 4`.

The checkout fetches full Git history because `AnyKeepMacro.cmake` derives the
application version from Git tags and the distance from the last tag.

## Package workflow

`.github/workflows/packages.yml` is the canonical package workflow.

- a version tag builds Debian, Windows, macOS, and Android packages;
- a manual run defaults to the stable Windows update channel and builds all
  package platforms;
- a manual run with `windows_update_channel=nightly` builds only Windows and
  runs the Windows tests first;
- the 03:00 UTC schedule is the former Windows nightly job: it builds only
  Windows with the nightly update channel and runs CTest before packaging.

The separate `windows-nightly.yml` workflow is intentionally no longer needed.
Keeping nightly and release Windows package assembly in one workflow prevents
the CMake flags, Store identity handling, dependency setup, and package targets
from drifting apart.

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