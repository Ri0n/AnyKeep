from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}: {old[:100]!r}")
    p.write_text(text.replace(old, new, 1))


# CMake: SDK-first QtKeychain resolution and Iris as the project default backend.
replace_once(
    "CMakeLists.txt",
    '''set(ANYKEEP_IRIS_SDK_ROOT
    ""
    CACHE PATH "Root of a prebuilt Iris SDK; when set it takes precedence over the bundled Iris fallback")
option(ANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME''',
    '''set(ANYKEEP_IRIS_SDK_ROOT
    ""
    CACHE PATH "Root of a prebuilt Iris SDK; when set it takes precedence over the bundled Iris fallback")
set(ANYKEEP_QTKEYCHAIN_SDK_ROOT
    ""
    CACHE PATH "Root of a prebuilt QtKeychain SDK; when set it takes precedence over installed/source fallbacks")
option(ANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME''',
)
replace_once(
    "CMakeLists.txt",
    '''set(ANYKEEP_XMPP_BACKEND
    "QXMPP"
    CACHE STRING "XMPP implementation used by the Private Notes plugin (QXMPP or IRIS)")''',
    '''set(ANYKEEP_XMPP_BACKEND
    "IRIS"
    CACHE STRING "XMPP implementation used by the Private Notes plugin (QXMPP or IRIS)")''',
)
replace_once(
    "CMakeLists.txt",
    '''foreach(_anykeep_sdk_root IN ITEMS "${ANYKEEP_QCA_SDK_ROOT}" "${ANYKEEP_IRIS_SDK_ROOT}")''',
    '''foreach(_anykeep_sdk_root IN ITEMS "${ANYKEEP_QCA_SDK_ROOT}" "${ANYKEEP_IRIS_SDK_ROOT}"
                                   "${ANYKEEP_QTKEYCHAIN_SDK_ROOT}")''',
)
replace_once(
    "CMakeLists.txt",
    '''find_package(Qt6Keychain QUIET)
if(NOT TARGET Qt6Keychain::Qt6Keychain AND ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN
   AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
  message(STATUS "System QtKeychain not found; using bundled QtKeychain")
  include(BundledQtKeychain)
elseif(NOT TARGET Qt6Keychain::Qt6Keychain AND ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN)
  message(STATUS "QtKeychain source fallback is disabled")
endif()''',
    '''if(ANYKEEP_QTKEYCHAIN_SDK_ROOT)
  find_package(Qt6Keychain CONFIG REQUIRED PATHS "${ANYKEEP_QTKEYCHAIN_SDK_ROOT}" NO_DEFAULT_PATH)
  if(NOT TARGET Qt6Keychain::Qt6Keychain)
    message(FATAL_ERROR "QtKeychain SDK did not provide Qt6Keychain::Qt6Keychain: ${ANYKEEP_QTKEYCHAIN_SDK_ROOT}")
  endif()
  message(STATUS "QtKeychain: using prebuilt SDK (${ANYKEEP_QTKEYCHAIN_SDK_ROOT})")
else()
  find_package(Qt6Keychain QUIET)
endif()
if(NOT TARGET Qt6Keychain::Qt6Keychain AND ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN
   AND ANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS)
  message(STATUS "System QtKeychain not found; using bundled QtKeychain")
  include(BundledQtKeychain)
elseif(NOT TARGET Qt6Keychain::Qt6Keychain AND ANYKEEP_BUILD_BUNDLED_QTKEYCHAIN)
  message(STATUS "QtKeychain source fallback is disabled")
endif()''',
)

# Normal CI should exercise Iris, not silently disable the XMPP plugin.
replace_once(
    ".github/workflows/ci.yml",
    '''            -DANYKEEP_UPDATE_CHANNEL=nightly `
            -DBUILD_TESTING=ON''',
    '''            -DANYKEEP_UPDATE_CHANNEL=nightly `
            -DANYKEEP_XMPP_BACKEND=IRIS `
            -DBUILD_TESTING=ON''',
)
replace_once(
    ".github/workflows/ci.yml",
    '''            -DCMAKE_BUILD_TYPE=Release \\
            -DANYKEEP_BUILD_BUNDLED_QCA=ON \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF''',
    '''            -DCMAKE_BUILD_TYPE=Release \\
            -DANYKEEP_XMPP_BACKEND=IRIS \\
            -DANYKEEP_BUILD_BUNDLED_QCA=ON \\
            -DANYKEEP_BUILD_BUNDLED_IRIS=ON \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF''',
)
replace_once(
    ".github/workflows/ci.yml",
    '''            -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR" \\
            -DANYKEEP_BUILD_BUNDLED_QCA=ON \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF''',
    '''            -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR" \\
            -DANYKEEP_XMPP_BACKEND=IRIS \\
            -DANYKEEP_BUILD_BUNDLED_QCA=ON \\
            -DANYKEEP_BUILD_BUNDLED_IRIS=ON \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF''',
)

# Packaging consumes versioned QCA/Iris/QtKeychain SDKs and is forbidden from
# falling back to configure/build-time network fetches.
replace_once(
    ".github/workflows/packages.yml",
    '''  QCA_TAG: v3.0.1
  IRIS_TAG: v1.0.2
  ANDROID_NDK_VERSION: r27c''',
    '''  QCA_TAG: v3.0.1
  IRIS_TAG: v1.0.2
  QTKEYCHAIN_TAG: deps-qtkeychain-0.14.3-r1-qt6.11
  ANDROID_NDK_VERSION: r27c''',
)
replace_once(
    ".github/workflows/packages.yml",
    '''      - name: Download QCA/Iris SDKs
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          New-Item -ItemType Directory -Force qca-sdk, iris-sdk | Out-Null
          gh release download $env:QCA_TAG --repo psi-im/qca -p qca3-qt6-windows-x64.zip -O qca.zip
          gh release download $env:IRIS_TAG --repo psi-im/iris -p iris-qt6-windows-x64.zip -O iris.zip
          # cmake -E tar has no destination argument; unpack in the SDK directories.
          Push-Location qca-sdk; cmake -E tar xvf ../qca.zip; Pop-Location
          Push-Location iris-sdk; cmake -E tar xvf ../iris.zip; Pop-Location
          if (-not (Test-Path qca-sdk/lib/cmake/Qca3-qt6)) { throw 'QCA CMake package missing' }
          if (-not (Test-Path iris-sdk/lib/cmake/Iris)) { throw 'Iris CMake package missing' }''',
    '''      - name: Download QCA/Iris/QtKeychain SDKs
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          New-Item -ItemType Directory -Force qca-sdk, iris-sdk, qtkeychain-sdk | Out-Null
          gh release download $env:QCA_TAG --repo psi-im/qca -p qca3-qt6-windows-x64.zip -O qca.zip
          gh release download $env:IRIS_TAG --repo psi-im/iris -p iris-qt6-windows-x64.zip -O iris.zip
          gh release download $env:QTKEYCHAIN_TAG --repo $env:GITHUB_REPOSITORY `
            -p qtkeychain-0.14.3-r1-qt6.11-windows-x64.tar.gz -O qtkeychain.tar.gz
          # cmake -E tar has no destination argument; unpack in the SDK directories.
          Push-Location qca-sdk; cmake -E tar xvf ../qca.zip; Pop-Location
          Push-Location iris-sdk; cmake -E tar xvf ../iris.zip; Pop-Location
          Push-Location qtkeychain-sdk; cmake -E tar xvf ../qtkeychain.tar.gz; Pop-Location
          if (-not (Test-Path qca-sdk/lib/cmake/Qca3-qt6)) { throw 'QCA CMake package missing' }
          if (-not (Test-Path iris-sdk/lib/cmake/Iris)) { throw 'Iris CMake package missing' }
          if (-not (Test-Path qtkeychain-sdk/lib/cmake/Qt6Keychain)) { throw 'QtKeychain CMake package missing' }''',
)
replace_once(
    ".github/workflows/packages.yml",
    '''            -DANYKEEP_IRIS_SDK_ROOT="$pwd/iris-sdk" `
            -DANYKEEP_BUILD_BUNDLED_QCA=OFF `
            -DANYKEEP_BUILD_BUNDLED_IRIS=OFF `
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF `
            -DANYKEEP_BUILD_BUNDLED_QCORO=OFF `
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON `
            -DANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME=ON `''',
    '''            -DANYKEEP_IRIS_SDK_ROOT="$pwd/iris-sdk" `
            -DANYKEEP_QTKEYCHAIN_SDK_ROOT="$pwd/qtkeychain-sdk" `
            -DANYKEEP_BUILD_BUNDLED_QCA=OFF `
            -DANYKEEP_BUILD_BUNDLED_IRIS=OFF `
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF `
            -DANYKEEP_BUILD_BUNDLED_QCORO=OFF `
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=OFF `
            -DANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS=OFF `
            -DANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME=ON `''',
)
replace_once(
    ".github/workflows/packages.yml",
    '''      - name: Download QCA/Iris SDKs
        shell: bash
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          set -euo pipefail
          mkdir qca-sdk iris-sdk
          gh release download "$QCA_TAG" --repo psi-im/qca \\
            -p "qca3-qt6-macos-${{ matrix.arch }}.zip" -O qca.zip
          gh release download "$IRIS_TAG" --repo psi-im/iris \\
            -p "iris-qt6-macos-${{ matrix.arch }}.zip" -O iris.zip
          (cd qca-sdk && cmake -E tar xvf ../qca.zip)
          (cd iris-sdk && cmake -E tar xvf ../iris.zip)
          test -d qca-sdk/lib/cmake/Qca3-qt6
          test -d iris-sdk/lib/cmake/Iris''',
    '''      - name: Download QCA/Iris/QtKeychain SDKs
        shell: bash
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          set -euo pipefail
          mkdir qca-sdk iris-sdk qtkeychain-sdk
          gh release download "$QCA_TAG" --repo psi-im/qca \\
            -p "qca3-qt6-macos-${{ matrix.arch }}.zip" -O qca.zip
          gh release download "$IRIS_TAG" --repo psi-im/iris \\
            -p "iris-qt6-macos-${{ matrix.arch }}.zip" -O iris.zip
          gh release download "$QTKEYCHAIN_TAG" --repo "$GITHUB_REPOSITORY" \\
            -p "qtkeychain-0.14.3-r1-qt6.11-macos-${{ matrix.arch }}.tar.gz" -O qtkeychain.tar.gz
          (cd qca-sdk && cmake -E tar xvf ../qca.zip)
          (cd iris-sdk && cmake -E tar xvf ../iris.zip)
          (cd qtkeychain-sdk && cmake -E tar xvf ../qtkeychain.tar.gz)
          test -d qca-sdk/lib/cmake/Qca3-qt6
          test -d iris-sdk/lib/cmake/Iris
          test -d qtkeychain-sdk/lib/cmake/Qt6Keychain''',
)
replace_once(
    ".github/workflows/packages.yml",
    '''            -DANYKEEP_IRIS_SDK_ROOT="$PWD/iris-sdk" \\
            -DANYKEEP_BUILD_BUNDLED_QCA=OFF \\
            -DANYKEEP_BUILD_BUNDLED_IRIS=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QCORO=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON \\
            -DANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME=ON \\''',
    '''            -DANYKEEP_IRIS_SDK_ROOT="$PWD/iris-sdk" \\
            -DANYKEEP_QTKEYCHAIN_SDK_ROOT="$PWD/qtkeychain-sdk" \\
            -DANYKEEP_BUILD_BUNDLED_QCA=OFF \\
            -DANYKEEP_BUILD_BUNDLED_IRIS=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QCORO=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=OFF \\
            -DANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS=OFF \\
            -DANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME=ON \\''',
)
replace_once(
    ".github/workflows/packages.yml",
    '''      - name: Download QCA/Iris SDKs
        shell: bash
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          set -euo pipefail
          mkdir qca-sdk iris-sdk
          gh release download "$QCA_TAG" --repo psi-im/qca \\
            -p "qca3-qt6-android-${{ matrix.abi }}.zip" -O qca.zip
          gh release download "$IRIS_TAG" --repo psi-im/iris \\
            -p "iris-qt6-android-${{ matrix.abi }}.zip" -O iris.zip
          (cd qca-sdk && cmake -E tar xvf ../qca.zip)
          (cd iris-sdk && cmake -E tar xvf ../iris.zip)
          test -d qca-sdk/lib/cmake/Qca3-qt6
          test -d iris-sdk/lib/cmake/Iris''',
    '''      - name: Download QCA/Iris/QtKeychain SDKs
        shell: bash
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          set -euo pipefail
          mkdir qca-sdk iris-sdk qtkeychain-sdk
          gh release download "$QCA_TAG" --repo psi-im/qca \\
            -p "qca3-qt6-android-${{ matrix.abi }}.zip" -O qca.zip
          gh release download "$IRIS_TAG" --repo psi-im/iris \\
            -p "iris-qt6-android-${{ matrix.abi }}.zip" -O iris.zip
          gh release download "$QTKEYCHAIN_TAG" --repo "$GITHUB_REPOSITORY" \\
            -p "qtkeychain-0.14.3-r1-qt6.11-android-${{ matrix.abi }}.tar.gz" -O qtkeychain.tar.gz
          (cd qca-sdk && cmake -E tar xvf ../qca.zip)
          (cd iris-sdk && cmake -E tar xvf ../iris.zip)
          (cd qtkeychain-sdk && cmake -E tar xvf ../qtkeychain.tar.gz)
          test -d qca-sdk/lib/cmake/Qca3-qt6
          test -d iris-sdk/lib/cmake/Iris
          test -d qtkeychain-sdk/lib/cmake/Qt6Keychain''',
)
replace_once(
    ".github/workflows/packages.yml",
    '''            -DANYKEEP_IRIS_SDK_ROOT="$PWD/iris-sdk" \\
            -DANYKEEP_BUILD_BUNDLED_QCA=OFF \\
            -DANYKEEP_BUILD_BUNDLED_IRIS=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QCORO=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=ON \\
            -DANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME=ON \\
            -DANYKEEP_ANDROID_OPENSSL_ROOT="$PWD/android-openssl/ssl_3" \\''',
    '''            -DANYKEEP_IRIS_SDK_ROOT="$PWD/iris-sdk" \\
            -DANYKEEP_QTKEYCHAIN_SDK_ROOT="$PWD/qtkeychain-sdk" \\
            -DANYKEEP_BUILD_BUNDLED_QCA=OFF \\
            -DANYKEEP_BUILD_BUNDLED_IRIS=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QXMPP=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QCORO=OFF \\
            -DANYKEEP_BUILD_BUNDLED_QTKEYCHAIN=OFF \\
            -DANYKEEP_ALLOW_SOURCE_DEPENDENCY_FALLBACKS=OFF \\
            -DANYKEEP_INSTALL_PREBUILT_SDK_RUNTIME=ON \\
            -DANYKEEP_ANDROID_OPENSSL_ROOT="$PWD/android-openssl/ssl_3" \\''',
)

print("QtKeychain SDK integration patch applied")
