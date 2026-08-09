#include <QtGlobal>

#include <QGuiApplication>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include "x11util.h"

void X11Util::forceActivateWindow(unsigned long winId)
{
    auto x11app = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11app) {
        return;
    }
    Display *display = x11app->display();
    if (!display) {
        return;
    }

    Atom   net_wm_active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    XEvent xev;
    xev.xclient.type         = ClientMessage;
    xev.xclient.send_event   = True;
    xev.xclient.display      = display;
    xev.xclient.window       = winId;
    xev.xclient.message_type = net_wm_active_window;
    xev.xclient.format       = 32;
    xev.xclient.data.l[0]    = 2;
    xev.xclient.data.l[1]    = CurrentTime;
    xev.xclient.data.l[2]    = 0;
    xev.xclient.data.l[3]    = 0;
    xev.xclient.data.l[4]    = 0;

    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);

    /* Ensure focus is actually switched to active window */
    XSetInputFocus(display, winId, RevertToParent, CurrentTime);
    XFlush(display);
}
