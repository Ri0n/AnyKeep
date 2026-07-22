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

#ifndef NOTESTORAGE_H
#define NOTESTORAGE_H

#include <QDateTime>
#include <QIcon>
#include <QObject> // just for compatibility with qt<4.6
#include <QPointer>
#include <QUrl>

#include "note.h"
#include "qtnote_export.h"
#include "storagejob.h"

namespace QtNote {

class NoteStorage;
class NoteFinder;
class SettingsController;

} // namespace QtNote

namespace QtNote {

class QTNOTE_EXPORT NoteStorage : public QObject {
    Q_OBJECT
public:
    using Ptr = QPointer<NoteStorage>;

    using QObject::QObject;
    virtual bool          init()               = 0;
    virtual const QString systemName() const   = 0;
    virtual const QString name() const         = 0;
    virtual QIcon         storageIcon() const  = 0;
    virtual QIcon         noteIcon() const     = 0;
    virtual bool          isAccessible() const = 0;

    virtual QList<Note::Format> availableFormats() const = 0;
    virtual bool                supportsMedia() const { return false; }

    /* 0 - not limit */
    virtual QList<Note> noteList(int limit = 0) = 0;

    /* should return null note (d=0) if not is not found */
    virtual Note note(const QString &id) = 0;

    virtual Note createNote() = 0;
    virtual bool loadNote(Note &note);
    virtual bool saveNote(const Note &note)        = 0;
    virtual void removeNote(const QString &noteId) = 0;

    virtual bool                isConfigurable() const { return false; }
    virtual QUrl                settingsComponent() const { return {}; }
    virtual SettingsController *createSettingsController(QObject *parent = nullptr)
    {
        Q_UNUSED(parent)
        return nullptr;
    }

    virtual QString tooltip() { return QString(); }

    // All consumers should use these methods. The synchronous API above is
    // retained temporarily while storage implementations are being migrated.
    virtual StorageInitJob *initAsync(QObject *owner = nullptr);
    virtual NoteListJob    *refreshNotesAsync(int limit = 0, QObject *owner = nullptr);
    virtual NoteLoadJob    *loadNoteAsync(const QString &id, QObject *owner = nullptr);
    virtual NoteSaveJob    *saveNoteAsync(const Note &note, QObject *owner = nullptr);
    virtual NoteRemoveJob  *removeNoteAsync(const QString &id, QObject *owner = nullptr);

    // Stops accepting work and synchronously drains implementation-owned workers.
    // Called before the storage object or its plugin code can be unloaded.
    virtual void shutdown() { }

signals:
    void noteAdded(const Note &);
    void noteModified(const Note &);
    void noteRemoved(const Note &);
    void noteIdChanged(const Note &note, const QString &oldNoteId);
    void invalidated();
    void storageErorr(const QString &);
};

} // namespace QtNote

#endif // NOTESTORAGE_H
