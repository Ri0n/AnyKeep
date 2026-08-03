# Conan dependency audit

Audit date: 2026-07-31

This audit covers the third-party libraries that AnyKeep either resolves through
`find_package()` or builds from source. Build tools and platform APIs such as
Git, Python, pkg-config, X11 and KDE Frameworks are intentionally left to the
host SDK/package manager.

| Dependency | AnyKeep requirement | ConanCenter result | Decision |
| --- | --- | --- | --- |
| Qt | Qt 6.4+ (6.11 on the current Android branch) | A current Qt 6 recipe exists | Keep the ordinary `find_package(Qt6 ...)` path. `CMakeDeps`/`CMakeToolchain` can provide the same package without AnyKeep-specific code. |
| Hunspell | 1.7.x | `hunspell/1.7.2` is available | Already consumed from `conanfile.txt`; keep the system-package fallback in the spellchecker plugin. |
| QSourceHighlite | pinned source revision | No matching ConanCenter recipe found | Try a configured CMake package first (including a private Conan recipe), then build the pinned revision through `ExternalProject`. |
| protobuf-c | 1.5.x runtime for libomemo-c | `protobuf-c/1.5.2` is available | Candidate for a later OMEMO build-stack migration. The current bundled libomemo-c pipeline expects protobuf-c and libomemo-c in one staging prefix and must remain internally consistent for cross-builds. |
| QXmpp | 1.11+ with OMEMO; bundled version 1.15.1 | ConanCenter exposes only 1.4.0 | Do not use the recipe; it is below the required API/version and does not replace the complete OMEMO target pair. |
| QCoro | 0.13 for the bundled path | ConanCenter exposes 0.4.0 and no binary packages | Do not use the recipe; retain `find_package(QCoro6)` plus the existing pinned fallback. |
| QCA | Qt 6/OpenSSL-compatible current source | No matching ConanCenter recipe found | Retain the system package and bundled ExternalProject paths. |
| QtKeychain | 0.14.x | No matching ConanCenter recipe found | Retain the system package and bundled ExternalProject paths. |
| libomemo-c | 0.5.1 | No matching ConanCenter recipe found | Retain pkg-config/system discovery and the paired bundled build. |

The QSourceHighlite integration deliberately does not add a Git checkout to the
main source tree. A package supplied by Conan or another package manager wins;
otherwise the pinned source revision is fetched and built in the build tree.
