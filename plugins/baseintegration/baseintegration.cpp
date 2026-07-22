#include <QAction>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtPlugin>

#include "baseintegration.h"
#include "baseintegrationtray.h"
#include "pluginhostinterface.h"
#include "qtnote_config.h"
#include "qxtglobalshortcut.h"
#include "stickynotewindow.h"

#ifdef QTNOTE_ENABLE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

namespace QtNote {

static const QLatin1String pluginId("base_de");
static const QLatin1String stickyGeometryGroup("baseintegration/stickyPresentations");

#ifdef QTNOTE_ENABLE_X11
static void prepareX11StickyWindowHints(QWidget *window)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("xcb"))
        return;
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        return;

    const ::Window xid        = window->winId();
    const Atom     atomType   = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom     typeNormal = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(display, xid, atomType, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&typeNormal), 1);

    const Atom atomState = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom states[]  = { XInternAtom(display, "_NET_WM_STATE_BELOW", False),
                             XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False),
                             XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False) };
    XChangeProperty(display, xid, atomState, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(states), 3);
    XFlush(display);
    XCloseDisplay(display);
}

static void applyX11StickyWindowHints(QWidget *window)
{
    if (!window || QGuiApplication::platformName() != QLatin1String("xcb"))
        return;
    Display *display = XOpenDisplay(nullptr);
    if (!display)
        return;

    const ::Window xid        = window->winId();
    const Atom     atomType   = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const Atom     typeNormal = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    XChangeProperty(display, xid, atomType, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(&typeNormal), 1);

    XEvent event {};
    event.xclient.type         = ClientMessage;
    event.xclient.window       = xid;
    event.xclient.message_type = XInternAtom(display, "_NET_WM_STATE", False);
    event.xclient.format       = 32;
    event.xclient.data.l[0]    = 1; // _NET_WM_STATE_ADD
    event.xclient.data.l[1]    = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
    event.xclient.data.l[2]    = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    event.xclient.data.l[1] = XInternAtom(display, "_NET_WM_STATE_BELOW", False);
    event.xclient.data.l[2] = None;
    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    XCloseDisplay(display);
}
#endif

//******************************************
// BaseIntegration
//******************************************
BaseIntegration::BaseIntegration(QObject *parent) :
    QObject(parent), tray(0), isWayland(qgetenv("XDG_SESSION_TYPE") == "wayland")
{
}

BaseIntegration::~BaseIntegration()
{
    for (auto window : std::as_const(stickyWindows))
        delete window;
}

int BaseIntegration::metadataVersion() const { return MetadataVersion; }

PluginMetadata BaseIntegration::metadata()
{
    PluginMetadata md;
    md.id          = pluginId;
    md.icon        = QIcon(":/icons/logo");
    md.name        = "Base Integration";
    md.description = tr("Provides fallback desktop environment integration");
    md.author      = "Sergei Ilinykh <rion4ik@gmail.com>";
    md.version     = 0x010100;       // plugin's version 0xXXYYZZPP
    md.minVersion  = 0x030002;       // minimum compatible version of QtNote
    md.maxVersion  = QTNOTE_VERSION; // maximum compatible version of QtNote
    md.homepage    = QUrl("http://ri0n.github.io/QtNote");
    // md.extra.insert("de", QStringList() << "KDE-4");
    return md;
}

void BaseIntegration::setHost(PluginHostInterface *host) { this->host = host; }

void BaseIntegration::activateWindow(QWindow *window)
{
    if (!window)
        return;
    window->showNormal();
    window->raise();
    window->requestActivate();
}

TrayImpl *BaseIntegration::initTray(Main *qtnote)
{
    tray = new BaseIntegrationTray(qtnote, host, this);
    return tray;
}

bool BaseIntegration::registerGlobalShortcut(const QString &id, const QKeySequence &key, QAction *action)
{
    if (!isWayland && !_shortcuts.contains(id)) {
        auto gs = new QxtGlobalShortcut(key, this);
        _shortcuts.insert(id, gs);
        connect(gs, SIGNAL(activated()), action, SLOT(trigger()));
        return true;
    }
    return false;
}

bool BaseIntegration::updateGlobalShortcut(const QString &id, const QKeySequence &key)
{
    auto sc = _shortcuts.value(id);
    if (sc) {
        return sc->setShortcut(key);
    }
    return false;
}

void BaseIntegration::setGlobalShortcutEnabled(const QString &id, bool enabled)
{
    auto sc = _shortcuts.value(id);
    if (sc) {
        sc->setEnabled(enabled);
    }
}

void BaseIntegration::initializeStickyNotes(StickyNotesServiceInterface *service) { stickyNotes = service; }

bool BaseIntegration::isStickyNotesAvailable() const { return stickyNotes != nullptr; }

bool BaseIntegration::presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry)
{
    if (stickyId.isNull() || !stickyNotes)
        return false;
    if (auto window = stickyWindows.value(stickyId)) {
        window->refresh();
        window->show();
        return true;
    }

    QSettings settings;
    settings.beginGroup(stickyGeometryGroup);
    QRect geometry = settings.value(stickyId.toString(QUuid::WithoutBraces)).toRect();
    settings.endGroup();
    if (!geometry.isValid())
        geometry = preferredGeometry;
    if (!geometry.isValid()) {
        geometry = QRect(QPoint(), QSize(300, 220));
        if (const auto *screen = QGuiApplication::primaryScreen())
            geometry.moveCenter(screen->availableGeometry().center());
    }

    const bool restoring = !preferredGeometry.isValid();
    auto      *window    = new StickyNoteWindow(stickyId, stickyNotes);
    if (restoring)
        window->setAttribute(Qt::WA_ShowWithoutActivating);
    window->setGeometry(geometry);
    connect(window, &StickyNoteWindow::geometryCommitted, this, [stickyId](const QRect &rect) {
        QSettings settings;
        settings.beginGroup(stickyGeometryGroup);
        settings.setValue(stickyId.toString(QUuid::WithoutBraces), rect);
    });
    connect(window, &QObject::destroyed, this, [this, stickyId] { stickyWindows.remove(stickyId); });
    stickyWindows.insert(stickyId, window);
#ifdef QTNOTE_ENABLE_X11
    prepareX11StickyWindowHints(window);
#endif
    window->show();
#ifdef QTNOTE_ENABLE_X11
    QPointer<StickyNoteWindow> guardedWindow(window);
    const auto                 applyHints = [guardedWindow] {
        if (guardedWindow)
            applyX11StickyWindowHints(guardedWindow);
    };
    QTimer::singleShot(0, window, applyHints);
    QTimer::singleShot(100, window, applyHints);
#endif
    return true;
}

bool BaseIntegration::dismissStickyNote(const QUuid &stickyId)
{
    auto window = stickyWindows.take(stickyId);
    if (window) {
        window->hide();
        window->deleteLater();
    }
    QSettings settings;
    settings.beginGroup(stickyGeometryGroup);
    settings.remove(stickyId.toString(QUuid::WithoutBraces));
    return true;
}

QUuid BaseIntegration::stickyNoteIdForPresentation(const QString &presentationId) const
{
    return QUuid(presentationId);
}

void BaseIntegration::notifyError(const QString &message)
{
    if (tray) {
        ((BaseIntegrationTray *)tray)->tray->showMessage(tr("Error"), message, QSystemTrayIcon::Warning, 5000);
    }
}

} // namespace QtNote
