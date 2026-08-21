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

#ifndef NOTESTORAGE_H
#define NOTESTORAGE_H

#include <QDateTime>
#include <QIcon>
#include <QObject> // just for compatibility with qt<4.6
#include <QPointer>
#include <QUrl>

#include "anykeep_export.h"
#include "foldercatalog.h"
#include "note.h"
#include "storagejob.h"

namespace AnyKeep {

class NoteStorage;
class NoteFinder;
class SettingsController;

} // namespace AnyKeep

namespace AnyKeep {

class ANYKEEP_EXPORT NoteStorage : public QObject {
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

    // Whether a new or existing draft may be routed to this storage. Remote
    // storages can accept durable writes while their connection is still being
    // established, even though they are not yet readable.
    virtual bool canAcceptWrites() const { return isAccessible(); }

    virtual QList<Note::Format> availableFormats() const = 0;
    virtual bool                supportsMedia() const { return false; }
    virtual bool                supportsFavorite() const { return false; }
    virtual bool                supportsNoteReordering() const { return requestedModificationTimeResolutionMs() > 0; }
    // Native folder support means the provider can persist a note's folder
    // assignment independently from its body.  Providers without it use the
    // encrypted application-local overlay instead.
    virtual bool supportsNativeFolders() const { return false; }
    // This opt-in is deliberately narrower than generic rule execution on a
    // loaded note. A provider returning true permits only the local,
    // folder-assignment overlay import pass; it never permits a rule to save,
    // move, or otherwise modify provider data while it is being read.
    virtual bool supportsFolderRuleOverlayImport() const { return false; }
    // Some providers can also transport stable UUID folder-tree records. The
    // returned snapshot is the provider's contribution to the global merge.
    virtual bool                  supportsNativeFolderCatalog() const { return false; }
    virtual FolderCatalogSnapshot nativeFolderCatalog() const { return {}; }
    // A provider with a damaged local index must not look like a valid empty
    // contribution: the global catalog manager uses this state to avoid
    // overwriting recoverable native data during import/synchronization.
    virtual bool    nativeFolderCatalogAvailable() const { return supportsNativeFolderCatalog(); }
    virtual QString nativeFolderCatalogErrorString() const { return {}; }
    // Smallest modification-time step that can survive a save and a reload.
    // Zero means that an explicit modification time is unsupported.
    virtual qint64 requestedModificationTimeResolutionMs() const { return 0; }

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
    virtual StorageInitJob      *initAsync(QObject *owner = nullptr);
    virtual NoteListJob         *refreshNotesAsync(int limit = 0, QObject *owner = nullptr);
    virtual NoteLoadJob         *loadNoteAsync(const QString &id, QObject *owner = nullptr);
    virtual NoteSaveJob         *saveNoteAsync(const Note &note, QObject *owner = nullptr);
    virtual NoteFolderChangeJob *changeNoteFolderAsync(const Note &note, QObject *owner = nullptr);
    virtual FolderCatalogJob    *replaceNativeFolderCatalogAsync(const FolderCatalogSnapshot &snapshot,
                                                                 QObject                     *owner = nullptr);
    virtual NoteRemoveJob       *removeNoteAsync(const QString &id, QObject *owner = nullptr);
    virtual NoteReorderJob      *reorderNoteAsync(const QString &noteId, const QString &afterNoteId,
                                                  QObject *owner = nullptr);
    virtual NoteReorderJob      *reorderNotesAsync(const QStringList &noteIds, const QString &afterNoteId,
                                                   QObject *owner = nullptr);

    // Stops accepting work and synchronously drains implementation-owned workers.
    // Called before the storage object or its plugin code can be unloaded.
    virtual void shutdown() {}

protected:
    struct NoteReorderChange {
        Note      note;
        QDateTime modified;
    };

    QList<NoteReorderChange> noteReorderChanges(const QStringList &noteIds, const QString &afterNoteId,
                                                StorageError *error = nullptr);

signals:
    void noteAdded(const Note &);
    void noteModified(const Note &);
    void noteRemoved(const Note &);
    void noteIdChanged(const Note &note, const QString &oldNoteId);
    void invalidated();
    void storageErorr(const QString &);
};

} // namespace AnyKeep

#endif // NOTESTORAGE_H
