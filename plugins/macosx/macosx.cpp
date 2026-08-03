/*
AnyKeep - Simple note-taking application
Copyright (C) 2010-2020 Sergey Ilinykh

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

#include <QProcess>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>
#include <QtPlugin>

#include "macosx.h"
#include "macosxtray.h"

namespace AnyKeep {
//------------------------------------------------------------
// MacOSXPlugin
//------------------------------------------------------------
MacOSXPlugin::MacOSXPlugin(QObject *parent) : QObject(parent), _tray(0) { }

void MacOSXPlugin::setHost(PluginHostInterface *host) { this->host = host; }

TrayImpl *MacOSXPlugin::initTray(Main *anykeep)
{
    _tray = new MacOSXTray(anykeep, this->host, this);
    return _tray;
}

void MacOSXPlugin::notifyError(const QString &msg)
{
    if (_tray) {
        _tray->sti->showMessage(tr("Error"), msg, QSystemTrayIcon::Warning);
    }
}

} // namespace AnyKeep
