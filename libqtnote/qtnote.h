/*
QtNote - Simple note-taking application
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

#ifndef QTNOTE_H
#define QTNOTE_H

#include <QObject>
#include <QUuid>
#include <functional>
#include <memory>

#include "notestorage.h"
#include "qtnote_export.h"

class QAction;
class QMenu;
class QRect;
class QWidget;
class QWindow;

namespace QtNote {

class PluginManager;
class ShortcutsManager;
class NoteWidget;
class TrayImpl;
class DEIntegrationInterface;
enum class WindowGeometryRestoreResult;
class GlobalShortcutsInterface;
class NotificationInterface;
class ActionNotificationInterface;
class NoteDialog;
class QtNoteDBus;
class Note;
class StickyNotesManager;
class StickyNotesIntegrationInterface;

class QTNOTE_EXPORT Main : public QObject {
    Q_OBJECT
public:
    explicit Main(QObject *parent = 0);
    ~Main();
    bool isOperable() const { return _inited; }
    void parseAppArguments(const QStringList &args);

    NoteWidget                 *noteWidget(const Note &note, const QUuid &draftId = {});
    virtual void                activateWidget(QWidget *w) const; // legacy desktop-shell adapter
    void                        activateWindow(QWindow *window) const;
    WindowGeometryRestoreResult restoreWindowGeometry(QWidget *w, const QString &key) const;
    WindowGeometryRestoreResult restoreWindowGeometry(QWindow *window, const QString &key) const;
    bool                        saveWindowGeometry(QWidget *w, const QString &key) const;
    bool                        saveWindowGeometry(QWindow *window, const QString &key) const;
    bool                        removeWindowGeometry(const QString &key) const;
    QString                     takePendingWindowGeometryKey() const;
    void                        windowGeometryBridgeReady() const;
    ShortcutsManager           *shortcutsManager() const { return _shortcutsManager; }
    PluginManager              *pluginManager() const { return _pluginManager; }
    StickyNotesManager         *stickyNotesManager() const;

    void setTrayImpl(TrayImpl *tray);
    void setExternalTrayAvailable(bool available);
    void setDesktopImpl(DEIntegrationInterface *de);
    void setGlobalShortcutsImpl(GlobalShortcutsInterface *gs);
    void setNotificationImpl(NotificationInterface *notifier);
    void setActionNotificationImpl(ActionNotificationInterface *notifier);
    void setStickyNotesImpl(StickyNotesIntegrationInterface *stickyNotes);

    void pinNote(const Note &note, const QUuid &draftId, bool awaitingPublication, const QRect &preferredGeometry);

    void registerStorage(std::unique_ptr<NoteStorage> storage);
    void unregisterStorage(NoteStorage *storage);

private:
    NoteDialog *makeNoteDialog(const QString &storageId, const QString &noteId = {});

signals:
    void noteWidgetCreated(QWidget *);
    void settingsUpdated();

public slots:
    void notifyError(const QString &);
    void notify(const QString &title, const QString &message, const QString &actionText, std::function<void()> action);
    void openNoteDialog(const QString &storageId, const QString &noteId);
    void appMessageReceived(const QString &message);

private slots:
    void exitQtNote();
    void showAbout();
    void showNoteManager();
    void showOptions();
    void createNewNote();
    void createNewNoteFromSelection();
    void note_trashRequested();
    void note_removed(const Note &noteItem);

private:
    class Private;
    Private *d;

    bool              _inited;
    ShortcutsManager *_shortcutsManager;
    PluginManager    *_pluginManager;
};

} // namespace QtNote

#endif // QTNOTE_H
