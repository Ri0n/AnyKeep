# Debian package profiles

AnyKeep uses separate Debian source-package profiles because APT resolves
`Build-Depends` before `debian/rules` is run. Qt major versions therefore
cannot be selected reliably with alternative dependencies in one control
file.

Available profiles:

- `qt6`: Qt 6, KF6 and Plasma 6 dependencies.
- `qt6-noble`: Qt 6 dependencies available in Ubuntu 24.04 Noble and Linux
  Mint 22.x. It uses system QXmpp 1.11 or newer when both the core and OMEMO
  components are available. Otherwise it builds QXmpp 1.15.1 with OMEMO as a
  bundled ExternalProject. Bundled QXmpp is linked statically into the XMPP
  plugin, so no private QXmpp shared libraries are shipped. KF6 integration
  and the Plasma 6 plasmoid are not built. The complete Noble profile is built
  with Clang because QXmpp's public coroutine templates also trigger GCC 13
  frontend crashes while compiling AnyKeep's XMPP plugin and tests.

Build binary and source packages with:

```sh
packaging/build-deb qt6-noble -us -uc
packaging/build-deb qt6 -us -uc
```

To create a source-only package for a PPA:

```sh
packaging/build-deb qt6-noble -S -sa
```

The selected Qt major is copied into `debian/build-profile.mk`, so it remains
part of the generated source package and is used later by the binary package
builder. The tracked `debian/control` and `debian/build-profile.mk` files are
restored when the command exits.

When a suitable system QXmpp is unavailable, the Noble profile downloads the
checksum-pinned official QXmpp release tarball during a local build. For a
network-isolated build, unpack the same release beforehand and configure with:

```sh
-DANYKEEP_BUILD_BUNDLED_QXMPP=ON \
-DANYKEEP_QXMPP_SOURCE_DIR=/path/to/qxmpp-1.15.1
```

Set `-DANYKEEP_BUNDLED_QXMPP_STATIC=OFF` to build and package private shared
QXmpp libraries instead.

Set `-DANYKEEP_ENABLE_X11=OFF` for a Wayland-only AnyKeep build. This removes the
GNOME plugin's X11 activation helper and disables the Linux base-integration
plugin, whose global-shortcut implementation requires X11. AnyKeep will not
search for or link directly to X11 in this mode.

For a Debian profile build, pass additional CMake options through the
environment, for example:

```sh
ANYKEEP_CMAKE_EXTRA_ARGS=-DANYKEEP_ENABLE_X11=OFF admin/debbuild.sh qt6-noble
```

An arbitrary package builder can be run under a selected profile after `--`:

```sh
packaging/build-deb qt6-noble -- gbp buildpackage -us -uc
```
