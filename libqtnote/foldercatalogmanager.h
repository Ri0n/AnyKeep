/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef FOLDERCATALOGMANAGER_H
#define FOLDERCATALOGMANAGER_H

#include "foldercatalog.h"
#include "qtnote_export.h"

#include <QObject>
#include <QSet>

#include <functional>
#include <memory>

namespace QtNote {

class FileFolderCatalogStore;
class NoteManager;
class NoteStorage;

/**
 * Owns the live, application-wide folder tree and its encrypted local
 * persistence.
 *
 * FolderCatalog is intentionally a value object.  This manager is the sole
 * mutation point for the global tree and for local overlay assignments used
 * by providers such as Tomboy.  A broken catalog does not stop the app: the
 * manager presents an empty read-only projection until the caller explicitly
 * restores a backup or recreates the catalog.
 */
class QTNOTE_EXPORT FolderCatalogManager final : public QObject {
    Q_OBJECT
public:
    static FolderCatalogManager *instance();

    explicit FolderCatalogManager(QObject *parent = nullptr);
    FolderCatalogManager(std::unique_ptr<FileFolderCatalogStore> store, QObject *parent = nullptr);
    ~FolderCatalogManager() override;

    /**
     * Opens the encrypted local catalog.  It returns false when the catalog
     * cannot be read, but leaves an empty safe projection available for
     * callers that only need to display Unsorted notes.
     */
    bool initialize(QString *errorText = nullptr);

    bool    isAvailable() const { return available_; }
    bool    needsRecovery() const { return needsRecovery_; }
    bool    hasRecoveryBackup() const;
    QString lastError() const { return lastError_; }

    const FolderCatalog         &catalog() const { return catalog_; }
    const FolderCatalogSnapshot &snapshot() const { return catalog_.snapshot(); }

    FolderCatalogResult<QUuid> addFolder(FolderRecord record);
    FolderCatalogError         updateFolder(FolderRecord record);
    FolderCatalogError         renameFolder(const QUuid &id, const QString &name);
    FolderCatalogError         moveFolder(const QUuid &id, const QUuid &parentId, qint64 sortOrder);
    FolderCatalogError         moveFolderRelative(const QUuid &id, const QUuid &parentId, const QUuid &beforeId = {});
    FolderCatalogError         setFolderCollapsed(const QUuid &id, bool collapsed);
    FolderCatalogError         setAllFoldersCollapsed(bool collapsed);
    FolderCatalogError         setFolderFlags(const QUuid &id, bool favorite, bool archived);
    FolderCatalogError         assignNote(const QString &storageId, const QString &noteId, const QUuid &folderId);
    FolderCatalogError         clearNoteAssignment(const QString &storageId, const QString &noteId);
    FolderCatalogError recycleNote(const QString &storageId, const QString &noteId, const QUuid &previousFolderId);
    FolderCatalogResult<QUuid> restoreRecycledNote(const QString &storageId, const QString &noteId);
    FolderCatalogError         reconcileProviderFolderPaths(const QString                             &storageId,
                                                            const QList<ProviderFolderPathAssignment> &assignments);

    /**
     * Merge one native provider contribution.  A provider may only submit
     * assignments for itself; folders are global and can legitimately be
     * shared with contributions from other providers.
     */
    FolderCatalogError mergeProviderCatalog(const QString &storageId, const FolderCatalogSnapshot &snapshot);

    /** Explicit recovery operations; neither is attempted automatically. */
    FolderCatalogError restoreBackup(QString *preservedPath = nullptr);
    FolderCatalogError recreate(QString *preservedPath = nullptr);

    /** Attach to storage readiness after application startup. */
    void observeNoteManager(NoteManager *manager);

signals:
    void catalogChanged();
    void availabilityChanged(bool available);
    void recoveryRequired(const QString &message, bool backupAvailable);
    void catalogError(const QString &message);
    void providerCatalogUnavailable(const QString &storageId, const QString &message);

private:
    using CatalogMutation = std::function<FolderCatalogError(FolderCatalog &)>;

    std::unique_ptr<FileFolderCatalogStore> store_;
    FolderCatalog                           catalog_;
    NoteManager                            *noteManager_ { nullptr };
    QSet<QString>                           readyStorageIds_;
    QSet<NoteStorage *>                     observedStorages_;
    QString                                 lastError_;
    bool                                    available_ { false };
    bool                                    needsRecovery_ { false };

    FolderCatalogError        mutate(const CatalogMutation &mutation);
    FolderCatalogError        replaceWith(FolderCatalog candidate);
    bool                      loadCurrentStore(QString *errorText = nullptr);
    void                      becomeUnavailable(const FolderCatalogError &error, bool clearProjection);
    void                      observeStorage(NoteStorage *storage);
    void                      importNativeCatalog(NoteStorage *storage);
    void                      importReadyNativeCatalogs();
    void                      notifyCatalogChanged();
    static bool               sameSnapshot(const FolderCatalogSnapshot &left, const FolderCatalogSnapshot &right);
    static FolderCatalogError unavailableError(const QString &message);
};

} // namespace QtNote

#endif // FOLDERCATALOGMANAGER_H
