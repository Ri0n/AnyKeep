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

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QMetaType>
#include <QSettings>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include "anykeep.h"
#include "macosxtray.h"
#include "notemanager.h"
#include "pluginhostinterface.h"
#include "utils.h"

typedef QPair<QString, QString> NoteIdent;
Q_DECLARE_METATYPE(NoteIdent)

namespace AnyKeep {

MacOSXTray::MacOSXTray(Main *anykeep, PluginHostInterface *host, QObject *parent) :
    TrayImpl(parent), anykeep(anykeep), host(host), contextMenu(0)
{
    menuUpdateTimer = new QTimer(this);
    menuUpdateTimer->setInterval(1000);
    menuUpdateTimer->setSingleShot(true);
    connect(menuUpdateTimer, SIGNAL(timeout()), SLOT(rebuildMenu()));

    sti = new QSystemTrayIcon(QIcon::fromTheme("anykeep", QIcon(":/icons/trayicon")), this);

    actQuit    = new QAction(QIcon(":/icons/exit"), tr("&Quit"), this);
    actNew     = new QAction(QIcon(":/icons/new"), tr("&New"), this);
    actAbout   = new QAction(QIcon(":/icons/trayicon"), tr("&About"), this);
    actOptions = new QAction(QIcon(":/icons/options"), tr("&Options"), this);
    actManager = new QAction(QIcon(":/icons/manager"), tr("&Note Manager"), this);

    connect(actQuit, SIGNAL(triggered()), SIGNAL(exitTriggered()));
    connect(actNew, SIGNAL(triggered()), SIGNAL(newNoteTriggered()));
    connect(actManager, SIGNAL(triggered()), SIGNAL(noteManagerTriggered()));
    connect(actOptions, SIGNAL(triggered()), SIGNAL(optionsTriggered()));
    connect(actAbout, SIGNAL(triggered()), SIGNAL(aboutTriggered()));

    advancedMenu = new QMenu;
    advancedMenu->addAction(actOptions);
    advancedMenu->addAction(actManager);
    advancedMenu->addAction(actAbout);
    advancedMenu->addSeparator();
    advancedMenu->addAction(actQuit);
    advancedMenu->setTitle(tr("More.."));

    connect(host->noteManager(), SIGNAL(storageAdded(NoteStorage::Ptr)), menuUpdateTimer, SLOT(start()));
    connect(host->noteManager(), SIGNAL(storageRemoved(NoteStorage::Ptr)), menuUpdateTimer, SLOT(start()));
    connect(host->noteManager(), SIGNAL(storageChanged(NoteStorage::Ptr)), menuUpdateTimer, SLOT(start()));
    menuUpdateTimer->start();
}

MacOSXTray::~MacOSXTray()
{
    delete contextMenu;
    delete advancedMenu;
}

void MacOSXTray::rebuildMenu()
{
    uint        h = 0;
    QSettings   s;
    QList<Note> notes = host->noteManager()->noteList(s.value("ui.menu-notes-amount", 15).toInt());
    for (int i = 0; i < notes.count(); i++) {
        h ^= qHash(notes[i].displayTitle());
    }

    if (h == menuUpdateHash) {
        return; // menu is not changed;
    }

    delete sti->contextMenu();
    QMenu *contextMenu = new QMenu(tr("Notes"));
    contextMenu->addAction(actNew);
    contextMenu->addSeparator();
    for (int i = 0; i < notes.count(); i++) {
        QAction *act = contextMenu->addAction(notes[i].storage()->noteIcon(),
                                              host->utilsCuttedDots(notes[i].displayTitle(), 48).replace('&', "&&"));
        QVariant v;
        v.setValue<NoteIdent>(NoteIdent(notes[i].storageId(), notes[i].id()));
        act->setData(v);
        connect(act, SIGNAL(triggered()), SLOT(noteSelected()));
    }
    if (notes.count()) {
        contextMenu->addSeparator();
    }
    contextMenu->addMenu(advancedMenu);

    sti->setContextMenu(contextMenu);
    sti->show();
}

void MacOSXTray::noteSelected()
{
    NoteIdent ni = ((QAction *)sender())->data().value<NoteIdent>();
    emit      showNoteTriggered(ni.first, ni.second);
}

} // namespace AnyKeep
