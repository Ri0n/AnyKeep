#ifndef NEXTCLOUDSTORAGE_H
#define NEXTCLOUDSTORAGE_H

#include "nextclouddto.h"
#include "notestorage.h"

#include <QHash>
#include <QSet>
#include <QThread>

namespace QtNote {

class NextcloudWorker;

class NextcloudStorage final : public NoteStorage {
    Q_OBJECT
    Q_DISABLE_COPY(NextcloudStorage)

public:
    explicit NextcloudStorage(QObject *parent = nullptr);
    ~NextcloudStorage() override;
    void shutdown() override;

    bool            init() override;
    StorageInitJob *initAsync(QObject *owner = nullptr) override;
    const QString   systemName() const override;
    const QString   name() const override;
    QIcon           storageIcon() const override;
    QIcon           noteIcon() const override;
    bool            isAccessible() const override;

    QList<Note::Format> availableFormats() const override;
    QList<Note>         noteList(int limit = 0) override;
    NoteListJob        *refreshNotesAsync(int limit = 0, QObject *owner = nullptr) override;
    Note                note(const QString &id) override;
    NoteLoadJob        *loadNoteAsync(const QString &id, QObject *owner = nullptr) override;
    Note                createNote() override;
    bool                loadNote(Note &note) override;
    bool                saveNote(const Note &note) override;
    NoteSaveJob        *saveNoteAsync(const Note &note, QObject *owner = nullptr) override;
    void                removeNote(const QString &noteId) override;
    NoteRemoveJob      *removeNoteAsync(const QString &noteId, QObject *owner = nullptr) override;

    bool                isConfigurable() const override { return true; }
    QUrl                settingsComponent() const override;
    SettingsController *createSettingsController(QObject *parent = nullptr) override;
    QString             tooltip() override;

    static QString storageId;

private:
    friend class NextcloudSettingsController;
    NextcloudConfig     readConfig() const;
    bool                configIsValid(const NextcloudConfig &config, QString *error) const;
    Note                fromRemote(const NextcloudRemoteNote &remote);
    void                applyRemote(Note &note, const NextcloudRemoteNote &remote);
    NextcloudRemoteNote toRemote(const Note &note) const;
    void                reportError(const QString &error, bool invalidate = false);
    void                applyConfig(const NextcloudConfig &config);

    NextcloudConfig      config_;
    QThread              workerThread_;
    NextcloudWorker     *worker_ { nullptr };
    QHash<QString, Note> cache_;
    // Successful writes may briefly be absent from an eventually consistent
    // list response. Keep them until the server snapshot acknowledges them.
    QHash<QString, Note> locallySaved_;
    QSet<QString>        locallyRemoved_;
    bool                 cacheValid_ { false };
    bool                 accessible_ { false };
};

} // namespace QtNote

#endif // NEXTCLOUDSTORAGE_H
