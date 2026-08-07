/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Contacts:
E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
*/

#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickStyle>
#include <QSocketNotifier>
#include <QStringList>
#include <QtSingleApplication>
#include <csignal>
#include <cstring>
#include <iostream>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#include "anykeep.h"

#ifdef Q_OS_UNIX
namespace {
int signalPipe[2] = { -1, -1 };

void handleUnixSignal(int)
{
    const char byte = 1;
    if (signalPipe[1] != -1) {
        const ssize_t written = ::write(signalPipe[1], &byte, sizeof(byte));
        (void)written;
    }
}

void installUnixSignalHandler(int signal)
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = handleUnixSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(signal, &action, nullptr);
}

QSocketNotifier *installUnixSignalHandlers(QObject *parent)
{
    if (::pipe(signalPipe) != 0)
        return nullptr;

    for (int fd : signalPipe) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags != -1)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    installUnixSignalHandler(SIGINT);
    installUnixSignalHandler(SIGTERM);

    auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, parent);
    QObject::connect(notifier, &QSocketNotifier::activated, qApp, [notifier]() {
        notifier->setEnabled(false);

        char buffer[16];
        while (::read(signalPipe[0], buffer, sizeof(buffer)) > 0) { }

        QApplication::quit();
    });
    return notifier;
}
}
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // The legacy Qt Quick Controls Windows style does not connect its palette
    // to the Windows dark colour scheme. FluentWinUI3 does and tracks the
    // system appearance for all Quick Controls, including popups and menus.
    QQuickStyle::setStyle(QStringLiteral("FluentWinUI3"));
#endif

    bool safeMode = false;
    for (int i = 1; i < argc; ++i) {
        const QLatin1String argument(argv[i]);
        if (argument == "--safe-mode" || argument == "--safemode")
            safeMode = true;
    }

    // Prefix every Qt log message, including messages from plugins and Qt
    // itself, with monotonic seconds elapsed since this process started.
    qSetMessagePattern(QStringLiteral("[%{time process}] %{message}"));

    for (int i = 1; i < argc; i++) {
        QLatin1String v(argv[i]);
        if (v == "-h" || v == "--help") {
            std::cout << "AnyKeep - note taking application\n\n"
                      << " -n [type]          Create a new note from 'type'. 'selection' is the only supported type.\n"
                      << " -m, --note-manager Open the Notes Manager.\n"
                      << " --safe-mode, --safemode\n"
                      << "                    Load only the base desktop integration plugin.\n\n";
            return 0;
        }
    }

    QtSingleApplication a(argc, argv); //, true, SingleApplication::Mode::User, 1000, "xxx");
    if (a.isRunning()) {
        if (safeMode) {
            std::cerr << "AnyKeep is already running. Stop it before starting with --safe-mode.\n";
            return 1;
        }
        QStringList args = a.arguments();
        if (args.size() > 1) {
            args.pop_front();
            a.sendMessage(args.join("!anykeep_argdelim!").toUtf8());
        }
        return 0;
    }

    QCoreApplication::setOrganizationName("R-Soft"); // get rid of useless dirs
    QCoreApplication::setApplicationName("AnyKeep");
    QGuiApplication::setApplicationDisplayName("AnyKeep");
    QGuiApplication::setDesktopFileName("anykeep");

    QApplication::setQuitOnLastWindowClosed(false);
    a.setProperty("anykeepSafeMode", safeMode);

#ifdef Q_OS_UNIX
    installUnixSignalHandlers(&a);
#endif

    AnyKeep::Main anykeep;
    if (anykeep.isOperable()) {
        a.connect(&a, &QtSingleApplication::messageReceived, &anykeep, &AnyKeep::Main::appMessageReceived);
        anykeep.parseAppArguments(a.arguments().mid(1));
        return a.exec();
    }
    return 1;
}
