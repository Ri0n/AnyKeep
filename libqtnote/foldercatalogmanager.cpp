/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "foldercatalogmanager.h"

#include "filefoldercatalogstore.h"
#include "localdatakeystore.h"
#include "notemanager.h"
#include "notestorage.h"
#include "utils.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <utility>

namespace QtNote {

Q_LOGGING_CATEGORY(logFolderCatalog, "qtnote.persistence.folders")

namespace {

    bool sameFolder(const FolderRecord &left, const FolderRecord &right)
    {
        return left.id == right.id && left.parentId == right.parentId && left.name == right.name
            && left.sortOrder == right.sortOrder && left.collapsed == right.collapsed && left.favorite == right.favorite
            && left.archived == right.archived && left.revision == right.revision && left.modifiedAt == right.modifiedAt
            && left.tombstone == right.tombstone;
    }

    bool sameAssignment(const NoteFolderAssignment &left, const NoteFolderAssignment &right)
    {
        return left.storageId == right.storageId && left.noteId == right.noteId && left.folderId == right.folderId
            && left.previousFolderId == right.previousFolderId && left.recycledAt == right.recycledAt
            && left.revision == right.revision && left.modifiedAt == right.modifiedAt
            && left.tombstone == right.tombstone;
    }

    bool samePathHint(const ProviderPathHint &left, const ProviderPathHint &right)
    {
        return left.storageId == right.storageId && left.path == right.path && left.folderId == right.folderId
            && left.revision == right.revision && left.modifiedAt == right.modifiedAt;
    }

} // namespace

FolderCatalogManager::FolderCatalogManager(QObject *parent) : QObject(parent) { }

FolderCatalogManager::FolderCatalogManager(std::unique_ptr<FileFolderCatalogStore> store, QObject *parent) :
    QObject(parent), store_(std::move(store))
{
}

FolderCatalogManager::~FolderCatalogManager() = default;

FolderCatalogManager *FolderCatalogManager::instance()
{
    static FolderCatalogManager *manager = new FolderCatalogManager(QCoreApplication::instance());
    return manager;
}

bool FolderCatalogManager::initialize(QString *errorText)
{
    if (available_) {
        if (errorText)
            errorText->clear();
        return true;
    }

    if (!store_) {
        QString keyError;
        auto    key = LocalDataKeyStore::loadOrCreateMasterKey(&keyError);
        if (key.isEmpty()) {
            becomeUnavailable({ FolderCatalogError::Locked, keyError }, true);
            if (errorText)
                *errorText = lastError_;
            return false;
        }
        const auto filePath = Utils::qtnoteDataDir() + QStringLiteral("/folder-catalog.bin");
        store_              = std::make_unique<FileFolderCatalogStore>(filePath, std::move(key));
    }

    return loadCurrentStore(errorText);
}

bool FolderCatalogManager::hasRecoveryBackup() const { return store_ && store_->hasBackup(); }

FolderCatalogResult<QUuid> FolderCatalogManager::addFolder(FolderRecord record)
{
    if (!available_)
        return { {}, unavailableError(lastError_) };

    FolderCatalog candidate;
    if (const auto validation = candidate.replaceSnapshot(catalog_.snapshot()))
        return { {}, validation };
    auto result = candidate.addFolder(std::move(record));
    if (!result)
        return result;
    if (const auto persistError = replaceWith(std::move(candidate)))
        return { {}, persistError };
    return result;
}

FolderCatalogError FolderCatalogManager::updateFolder(FolderRecord record)
{
    return mutate([record = std::move(record)](FolderCatalog &catalog) mutable {
        return catalog.updateFolder(std::move(record));
    });
}

FolderCatalogError FolderCatalogManager::renameFolder(const QUuid &id, const QString &name)
{
    return mutate([id, name](FolderCatalog &catalog) { return catalog.renameFolder(id, name); });
}

FolderCatalogError FolderCatalogManager::moveFolder(const QUuid &id, const QUuid &parentId, qint64 sortOrder)
{
    return mutate(
        [id, parentId, sortOrder](FolderCatalog &catalog) { return catalog.moveFolder(id, parentId, sortOrder); });
}

FolderCatalogError FolderCatalogManager::moveFolderRelative(const QUuid &id, const QUuid &parentId,
                                                            const QUuid &beforeId)
{
    return mutate([id, parentId, beforeId](FolderCatalog &catalog) {
        return catalog.moveFolderRelative(id, parentId, beforeId);
    });
}

FolderCatalogError FolderCatalogManager::setFolderCollapsed(const QUuid &id, bool collapsed)
{
    return mutate([id, collapsed](FolderCatalog &catalog) { return catalog.setFolderCollapsed(id, collapsed); });
}

FolderCatalogError FolderCatalogManager::setAllFoldersCollapsed(bool collapsed)
{
    return mutate([collapsed](FolderCatalog &catalog) { return catalog.setAllFoldersCollapsed(collapsed); });
}

FolderCatalogError FolderCatalogManager::setFolderFlags(const QUuid &id, bool favorite, bool archived)
{
    return mutate(
        [id, favorite, archived](FolderCatalog &catalog) { return catalog.setFolderFlags(id, favorite, archived); });
}

FolderCatalogError FolderCatalogManager::assignNote(const QString &storageId, const QString &noteId,
                                                    const QUuid &folderId)
{
    return mutate([storageId, noteId, folderId](FolderCatalog &catalog) {
        return catalog.assignNote(storageId, noteId, folderId);
    });
}

FolderCatalogError FolderCatalogManager::clearNoteAssignment(const QString &storageId, const QString &noteId)
{
    return mutate(
        [storageId, noteId](FolderCatalog &catalog) { return catalog.clearNoteAssignment(storageId, noteId); });
}

FolderCatalogError FolderCatalogManager::recycleNote(const QString &storageId, const QString &noteId,
                                                      const QUuid &previousFolderId)
{
    return mutate([storageId, noteId, previousFolderId](FolderCatalog &catalog) {
        return catalog.recycleNote(storageId, noteId, previousFolderId);
    });
}

FolderCatalogResult<QUuid> FolderCatalogManager::restoreRecycledNote(const QString &storageId, const QString &noteId)
{
    if (!available_)
        return { {}, unavailableError(lastError_) };
    FolderCatalog candidate;
    if (const auto validation = candidate.replaceSnapshot(catalog_.snapshot()))
        return { {}, validation };
    const auto result = candidate.restoreRecycledNote(storageId, noteId);
    if (!result)
        return result;
    if (const auto persistError = replaceWith(std::move(candidate)))
        return { {}, persistError };
    return result;
}

FolderCatalogError
FolderCatalogManager::reconcileProviderFolderPaths(const QString                             &storageId,
                                                   const QList<ProviderFolderPathAssignment> &assignments)
{
    return mutate([storageId, assignments](FolderCatalog &catalog) {
        return catalog.reconcileProviderFolderPaths(storageId, assignments);
    });
}

FolderCatalogError FolderCatalogManager::mergeProviderCatalog(const QString               &storageId,
                                                              const FolderCatalogSnapshot &snapshot)
{
    if (storageId.isEmpty())
        return { FolderCatalogError::InvalidArgument, tr("Storage id is required for a folder catalog import") };
    for (const auto &assignment : snapshot.assignments) {
        if (assignment.storageId != storageId) {
            return { FolderCatalogError::InvalidArgument,
                     tr("A storage submitted a folder assignment owned by another storage") };
        }
    }
    for (const auto &hint : snapshot.pathHints) {
        if (hint.storageId != storageId) {
            return { FolderCatalogError::InvalidArgument,
                     tr("A storage submitted a folder path hint owned by another storage") };
        }
    }
    return mutate([snapshot](FolderCatalog &catalog) { return catalog.merge(snapshot); });
}

FolderCatalogError FolderCatalogManager::restoreBackup(QString *preservedPath)
{
    if (preservedPath)
        preservedPath->clear();
    if (!store_)
        return unavailableError(lastError_);
    if (const auto restoreError = store_->restoreBackup(preservedPath)) {
        becomeUnavailable(restoreError, restoreError.code == FolderCatalogError::Corrupt);
        return restoreError;
    }
    return loadCurrentStore() ? FolderCatalogError {} : unavailableError(lastError_);
}

FolderCatalogError FolderCatalogManager::recreate(QString *preservedPath)
{
    if (preservedPath)
        preservedPath->clear();
    if (!store_)
        return unavailableError(lastError_);
    if (const auto recreateError = store_->recreate(preservedPath)) {
        becomeUnavailable(recreateError, recreateError.code == FolderCatalogError::Corrupt);
        return recreateError;
    }
    return loadCurrentStore() ? FolderCatalogError {} : unavailableError(lastError_);
}

void FolderCatalogManager::observeNoteManager(NoteManager *manager)
{
    if (noteManager_ == manager)
        return;
    if (noteManager_)
        disconnect(noteManager_, nullptr, this, nullptr);

    noteManager_ = manager;
    readyStorageIds_.clear();
    if (!noteManager_)
        return;

    connect(noteManager_, &NoteManager::storageReady, this, [this](const NoteStorage::Ptr &storage) {
        if (!storage)
            return;
        readyStorageIds_.insert(storage->systemName());
        importNativeCatalog(storage.data());
    });
    connect(noteManager_, &NoteManager::storageRemoved, this, [this](const NoteStorage::Ptr &storage) {
        if (storage)
            readyStorageIds_.remove(storage->systemName());
    });

    // Main attaches before core storages are registered.  The loop also makes
    // explicit attachment safe for callers that do it after a storage is up.
    for (const auto &storage : noteManager_->storages()) {
        if (!storage || !storage->isAccessible())
            continue;
        readyStorageIds_.insert(storage->systemName());
    }
    importReadyNativeCatalogs();
}

FolderCatalogError FolderCatalogManager::mutate(const CatalogMutation &mutation)
{
    if (!available_)
        return unavailableError(lastError_);

    FolderCatalog candidate;
    if (const auto validation = candidate.replaceSnapshot(catalog_.snapshot()))
        return validation;
    if (const auto mutationError = mutation(candidate))
        return mutationError;
    return replaceWith(std::move(candidate));
}

FolderCatalogError FolderCatalogManager::replaceWith(FolderCatalog candidate)
{
    if (!available_)
        return unavailableError(lastError_);
    if (sameSnapshot(catalog_.snapshot(), candidate.snapshot()))
        return {};
    if (const auto saveError = store_->save(candidate.snapshot())) {
        // A corrupt primary means writing a new catalog would discard the
        // user's recoverable state.  Show an empty projection until an
        // explicit recovery action is selected instead.
        becomeUnavailable(saveError, saveError.code == FolderCatalogError::Corrupt);
        return saveError;
    }
    catalog_ = std::move(candidate);
    notifyCatalogChanged();
    return {};
}

bool FolderCatalogManager::loadCurrentStore(QString *errorText)
{
    Q_ASSERT(store_);
    const auto loaded = store_->load();
    if (!loaded) {
        becomeUnavailable(loaded.error, true);
        if (errorText)
            *errorText = lastError_;
        return false;
    }

    FolderCatalog candidate;
    if (const auto validation = candidate.replaceSnapshot(loaded.value)) {
        becomeUnavailable(validation, true);
        if (errorText)
            *errorText = lastError_;
        return false;
    }

    const bool projectionChanged      = !sameSnapshot(catalog_.snapshot(), candidate.snapshot());
    const bool availabilityWasChanged = !available_;
    catalog_                          = std::move(candidate);
    available_                        = true;
    needsRecovery_                    = false;
    lastError_.clear();
    if (errorText)
        errorText->clear();
    if (availabilityWasChanged)
        emit availabilityChanged(true);
    if (projectionChanged)
        notifyCatalogChanged();
    importReadyNativeCatalogs();
    return true;
}

void FolderCatalogManager::becomeUnavailable(const FolderCatalogError &error, bool clearProjection)
{
    const bool availabilityWasChanged = available_;
    const bool projectionChanged
        = clearProjection && (!catalog_.folders().isEmpty() || !catalog_.assignments().isEmpty());
    available_     = false;
    needsRecovery_ = error.code == FolderCatalogError::Corrupt;
    lastError_     = error.message.isEmpty() ? tr("The encrypted folder catalog is unavailable") : error.message;
    if (clearProjection)
        catalog_ = {};
    qCWarning(logFolderCatalog) << "Folder catalog is unavailable:" << lastError_
                                << "backupAvailable=" << hasRecoveryBackup();
    if (availabilityWasChanged)
        emit availabilityChanged(false);
    if (needsRecovery_)
        emit recoveryRequired(lastError_, hasRecoveryBackup());
    emit catalogError(lastError_);
    if (projectionChanged)
        notifyCatalogChanged();
}

void FolderCatalogManager::importNativeCatalog(NoteStorage *storage)
{
    if (!storage || !storage->supportsNativeFolderCatalog())
        return;
    if (!storage->nativeFolderCatalogAvailable()) {
        const auto error = storage->nativeFolderCatalogErrorString().isEmpty()
            ? tr("The storage folder catalog is unavailable")
            : storage->nativeFolderCatalogErrorString();
        qCWarning(logFolderCatalog) << "Native folder catalog is unavailable:" << storage->systemName() << error;
        emit providerCatalogUnavailable(storage->systemName(), error);
        return;
    }
    if (const auto mergeError = mergeProviderCatalog(storage->systemName(), storage->nativeFolderCatalog())) {
        qCWarning(logFolderCatalog) << "Failed to import native folder catalog from" << storage->systemName() << ':'
                                    << mergeError.message;
        emit catalogError(mergeError.message);
    }
}

void FolderCatalogManager::importReadyNativeCatalogs()
{
    if (!noteManager_ || !available_)
        return;
    const auto storages = noteManager_->storages();
    for (const auto &storage : storages) {
        if (storage && readyStorageIds_.contains(storage->systemName()))
            importNativeCatalog(storage.data());
    }
}

void FolderCatalogManager::notifyCatalogChanged()
{
    qCInfo(logFolderCatalog) << "Folder catalog updated: folders=" << catalog_.folders().size()
                             << "assignments=" << catalog_.assignments().size()
                             << "pathHints=" << catalog_.pathHints().size();
    emit catalogChanged();
}

bool FolderCatalogManager::sameSnapshot(const FolderCatalogSnapshot &left, const FolderCatalogSnapshot &right)
{
    if (left.folders.size() != right.folders.size() || left.assignments.size() != right.assignments.size()
        || left.pathHints.size() != right.pathHints.size()) {
        return false;
    }
    for (qsizetype index = 0; index < left.folders.size(); ++index) {
        if (!sameFolder(left.folders.at(index), right.folders.at(index)))
            return false;
    }
    for (qsizetype index = 0; index < left.assignments.size(); ++index) {
        if (!sameAssignment(left.assignments.at(index), right.assignments.at(index)))
            return false;
    }
    for (qsizetype index = 0; index < left.pathHints.size(); ++index) {
        if (!samePathHint(left.pathHints.at(index), right.pathHints.at(index)))
            return false;
    }
    return true;
}

FolderCatalogError FolderCatalogManager::unavailableError(const QString &message)
{
    return { FolderCatalogError::Locked,
             message.isEmpty() ? tr("The encrypted folder catalog is unavailable") : message };
}

} // namespace QtNote
