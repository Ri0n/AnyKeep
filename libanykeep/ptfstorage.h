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

#ifndef PTFSTORAGE_H
#define PTFSTORAGE_H

#include "filestorage.h"
#include "foldercatalog.h"

namespace AnyKeep {

class LocalMediaStore;

class ANYKEEP_EXPORT PTFStorage : public FileStorage {
    Q_OBJECT
    Q_DISABLE_COPY(PTFStorage)

    QIcon icon;

public:
    PTFStorage(QObject *parent = 0);
    explicit PTFStorage(LocalMediaStore &mediaStore, QObject *parent = nullptr);
    bool          init() override;
    bool          isAccessible() const override;
    const QString systemName() const override;
    const QString name() const override;
    QIcon         storageIcon() const override;
    QIcon         noteIcon() const override;
    QList<Note>   noteListFromInfoList(const QFileInfoList &) override;

    Note createNote() override;
    Note note(const QString &noteId) override;
    bool loadNote(Note &note) override;
    bool saveNote(const Note &note) override;
    void removeNote(const QString &noteId) override;

    bool                  supportsNativeFolders() const override { return true; }
    bool                  supportsNativeFolderCatalog() const override { return true; }
    FolderCatalogSnapshot nativeFolderCatalog() const override;
    bool                  nativeFolderCatalogAvailable() const override { return folderCatalogAvailable_; }
    QString               nativeFolderCatalogErrorString() const override { return folderCatalogError_; }
    NoteFolderChangeJob  *changeNoteFolderAsync(const Note &note, QObject *owner = nullptr) override;
    FolderCatalogJob     *replaceNativeFolderCatalogAsync(const FolderCatalogSnapshot &snapshot,
                                                          QObject                     *owner = nullptr) override;

    bool               folderCatalogAvailable() const { return folderCatalogAvailable_; }
    QString            folderCatalogErrorString() const { return folderCatalogError_; }
    FolderCatalogError restoreFolderCatalogBackup(QString *preservedPath = nullptr);
    FolderCatalogError recreateFolderCatalog(QString *preservedPath = nullptr);

    QList<Note::Format> availableFormats() const override;
    qint64              requestedModificationTimeResolutionMs() const override { return 1; }
    NoteReorderJob     *reorderNotesAsync(const QStringList &noteIds, const QString &afterNoteId,
                                          QObject *owner = nullptr) override;
    bool                supportsMedia() const override { return true; }
    QString             findStorageDir() const override;

    static QString storageId;

private:
    void               loadFolderCatalog();
    QUuid              folderIdForNote(const QString &noteId) const;
    FolderCatalogError replaceFolderCatalog(const FolderCatalogSnapshot &snapshot);
    FolderCatalogError updateFolderAssignment(const QString &oldNoteId, const Note &saved);

    FolderCatalogSnapshot folderCatalog_;
    bool                  folderCatalogAvailable_ { true };
    QString               folderCatalogError_;
    LocalMediaStore      &mediaStore_;
};

} // namespace AnyKeep

#endif // PTFSTORAGE_H
