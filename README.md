QtNote
======
It's just very small Qt app which usually lives in your system tray and allows you to make notes quick way.

QtNote was written as a clone of Tomboy with use of Qt.
It's not so feature rich as Tomboy but light and fast and usually suits all common needs.
Moreover QtNote can work with Tomboy notes and it's not so hard add support of other apps.

Some features:
* Quick access to notes from tray menu
* Internal notes manager to handle multiple notes at once / search notes
* Support for Tomboy/Gnote notes
* Support for markdown
* Create notes from selection by a hotkey
* Spell checking (hunspell)
* Cross-platform (tested on Linux and Windows with gcc and Visual Studio)
* Other: configurable amount of notes in menu. configurable storage path, print note, save note dialog geometry


## Downloads

Check https://github.com/Ri0n/QtNote/releases page for the latest downloads.

Some older releases could be found at https://yadi.sk/d/HbnqnaTN6fwzN.

## Building

QtNote requires:

* a C++20 compiler;
* CMake 3.25 or newer;
* Qt 6.4 or newer with the Core, Gui, Widgets, PrintSupport, Network, and XML
  modules;
* QCA 2 built for Qt 6;
* QtKeychain built for Qt 6.

The default Linux configuration also uses Qt DBus and Qml. Qt Test is required
when configuring with `BUILD_TESTING=ON`, which is the default.

Optional dependencies enable additional features and plugins:

* Qt Multimedia for audio recording;
* Xlib development files for `baseintegration` and other direct X11 support
  when `QTNOTE_ENABLE_X11=ON` (the default on Linux);
* Hunspell for spell checking;
* ECM, KDE Frameworks 6 (Config, CoreAddons, GlobalAccel, Notifications, and
  WindowSystem), and Plasma 6 development files for KDE integration;
* QXmpp 1.11 or newer with OMEMO, QCoro, and Qt Network/XML for XMPP PubSub
  storage. Alternatively, configure with `QTNOTE_BUILD_BUNDLED_QXMPP=ON`; the
  bundled build additionally requires OpenSSL 3 and `libomemo-c`.

On Plasma 5 systems, use an X11 session with `baseintegration`. The native KDE
integration targets Qt 6, KDE Frameworks 6, and Plasma 6; Qt 5/KF5 builds are
not supported.

Configure and build QtNote with:

```bash
cmake -S . -B build
cmake --build build -j
cmake --install build
```

For the experimental Qt Quick Android application and Qt Creator kit setup,
see [Android build](docs/android-build.md).

The install step is optional. Its default prefix is normally `/usr/local`; set
another one during configuration with, for example,
`-DCMAKE_INSTALL_PREFIX=/usr`.

To list QtNote-specific configuration options after configuring the build
directory, run:

```bash
cmake -LA -N build | grep QTNOTE
```

The principal options are:

* `QTNOTE_DEVEL` — use development resource and plugin search paths;
* `QTNOTE_ENABLE_X11` — build components which access X11 directly;
* `QTNOTE_BUILD_BUNDLED_QXMPP` — build the bundled QXmpp fallback;
* `QTNOTE_PLUGIN_ENABLE_<name>` — enable or disable an individual plugin.
  Plugins default to enabled when the platform and their dependencies support
  them.

## Build DEB/RPM

Check ./admin/{deb,rpm}build.sh scripts. You can start them w/o arguments.

## Build on Microsoft Windows

You need conan in your PATH. It's up to you how you install it. Then you need to enable
Conan support in Qt Creator plugins. You need cmake and ninja in PATH too because
Qt Creator doesn't pass their locations to Conan (just add those installed by
Qt Maintenance tool). The remaining magic should work automatically.

### Creating an installer

To build an installer wix.exe from WiX toolset also has to be in PATH.

Follow next steps after installing dependencies and configuring PATH:

1. select release build in qt creator
2. build it (ctrl + b)
3. then update deployment configuration and 2 steps in exact order
   * cmake install
   * cmake build of `burn_installer` target
4. try to run and this will execute the deployment.
5. deployment creates QtNote installer, so check terminal for logs.

## Internationalization

https://app.transifex.com/rion/qtnote

## Architecture documentation

- [Media storage, encrypted local blobs, drafts, and remote caches](docs/media-storage-architecture.md)
- [QML editor Markdown compatibility](docs/markdown-support.md)

### Planned major featues

1. note encryption (QCA-based)
2. AI-based classification
3. more plugins for various popular systems

## Credits and Third-Party Assets
* **Icons**: Select icons used in this project are sourced from the [KDE Breeze Icons](https://kde.org) theme, licensed under the [GNU LGPLv2.1+](https://gnu.org).
