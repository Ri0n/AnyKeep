#ifndef NOTESINDEX_H
#define NOTESINDEX_H

#include "anykeep_export.h"
#include "notestorage.h"

#include <QHash>
#include <QPointer>

#include <list>

namespace AnyKeep {

class ANYKEEP_EXPORT NotesIndex final : public QObject {
    Q_OBJECT
public:
    explicit NotesIndex(QObject *parent = nullptr);

    void addStorage(const NoteStorage::Ptr &storage);
    void removeStorage(NoteStorage *storage);
    void markStorageReady(const NoteStorage::Ptr &storage);
    void markStorageInitializationFailed(const NoteStorage::Ptr &storage, const StorageError &error);

    void         refreshStorage(const QString &storageId);
    NoteListJob *refreshAll(const std::list<NoteStorage::Ptr> &storages, int count = -1, QObject *owner = nullptr);

    QList<Note> notes(const QString &storageId) const;
    QList<Note> allNotes(const std::list<NoteStorage::Ptr> &storages, int count = -1) const;
    int         noteCount(const QString &storageId) const;
    bool        isLoading(const QString &storageId) const;
    bool        hasSnapshot(const QString &storageId) const;
    QString     errorString(const QString &storageId) const;

signals:
    void storageNotesChanged(const QString &storageId);
    void storageStateChanged(const QString &storageId);

private:
    struct StorageState {
        NoteStorage::Ptr      storage;
        QList<Note>           notes;
        QPointer<NoteListJob> refreshJob;
        QString               errorString;
        quint64               refreshGeneration { 0 };
        bool                  initializationFinished { false };
        bool                  ready { false };
        bool                  loading { false };
        bool                  refreshPending { false };
        bool                  snapshotValid { false };
    };

    NoteListJob        *startRefresh(StorageState &state);
    void                upsertNote(const Note &note, const QString &oldNoteId = {});
    void                removeNote(const Note &note);
    StorageState       *stateForStorage(const QString &storageId);
    const StorageState *stateForStorage(const QString &storageId) const;

    QHash<QString, StorageState> states_;
};

} // namespace AnyKeep

#endif // NOTESINDEX_H
