/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "folderoperationscontroller.h"

#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notestorage.h"
#include "storagejob.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QTimer>

#include <algorithm>

namespace AnyKeep {

Q_LOGGING_CATEGORY(logFolderOperations, "anykeep.persistence.folderoperations")

namespace {
    QString diagnosticNoteId(const QString &noteId)
    {
        return noteId.size() > 16 ? noteId.left(16) + QStringLiteral("…") : noteId;
    }
}

FolderOperationsController::FolderOperationsController(FolderCatalogManager *catalogManager, NoteManager *noteManager,
                                                       QObject *parent) :
    QObject(parent), catalogManager_(catalogManager ? catalogManager : FolderCatalogManager::instance()),
    noteManager_(noteManager ? noteManager : NoteManager::instance())
{
}

FolderOperationsController *FolderOperationsController::instance()
{
    static FolderOperationsController *controller = new FolderOperationsController(
        FolderCatalogManager::instance(), NoteManager::instance(), QCoreApplication::instance());
    return controller;
}

bool FolderOperationsController::assignNoteFolder(const QString &storageId, const QString &noteId,
                                                  const QUuid &folderId, bool overlayAlreadyStored)
{
    qCInfo(logFolderOperations) << "Folder assignment requested: storage=" << storageId
                                << "note=" << diagnosticNoteId(noteId)
                                << "folder=" << folderId.toString(QUuid::WithoutBraces);
    if (storageId.isEmpty() || noteId.isEmpty()) {
        setError(tr("A storage and note are required to assign a folder"));
        return false;
    }
    auto storage = noteManager_->storage(storageId);
    if (!storage) {
        setError(tr("The note storage is no longer available"));
        return false;
    }
    Note note;
    for (const auto &candidate : noteManager_->notesIndex()->notes(storageId)) {
        if (candidate.id() == noteId) {
            note = candidate;
            break;
        }
    }
    if (note.isNull())
        note = storage->note(noteId);
    if (note.isNull()) {
        if (storage->supportsNativeFolders()) {
            // Remote caches can temporarily drop an entry while their index is
            // being refreshed or replaced after publication. Resolve the note
            // through the storage's asynchronous loader before declaring the
            // folder operation invalid.
            startAssignmentLoad(storage, noteId, folderId, overlayAlreadyStored);
            return true;
        }
        setError(tr("The note used for the folder change was not found"));
        return false;
    }

    if (!overlayAlreadyStored && !storeOverlayAssignment(storageId, noteId, folderId))
        return false;
    qCInfo(logFolderOperations) << "Folder overlay stored: storage=" << storageId << "note=" << diagnosticNoteId(noteId)
                                << "nativeFolders=" << storage->supportsNativeFolders();
    note.setFolderId(folderId);
    if (!storage->supportsNativeFolders()) {
        qCInfo(logFolderOperations) << "Folder assignment completed as local overlay: storage=" << storageId
                                    << "note=" << diagnosticNoteId(noteId);
        emit assignmentFinished(storageId, noteId, folderId, true);
        return true;
    }

    startNativeAssignment(storage, note, folderId);
    return true;
}

bool FolderOperationsController::storeOverlayAssignment(const QString &storageId, const QString &noteId,
                                                        const QUuid &folderId)
{
    if (storageId.isEmpty() || noteId.isEmpty()) {
        setError(tr("A storage and note are required to assign a folder"));
        return false;
    }
    if (!catalogManager_->isAvailable()) {
        setError(catalogManager_->lastError());
        return false;
    }
    if (!folderId.isNull() && !catalogManager_->catalog().folder(folderId)) {
        setError(tr("The selected folder no longer exists"));
        return false;
    }
    return updateOverlay(storageId, noteId, folderId);
}

bool FolderOperationsController::prepareNativeFolderTree(const QString &storageId)
{
    if (!catalogManager_->isAvailable()) {
        setError(catalogManager_->lastError());
        return false;
    }
    auto storage = noteManager_->storage(storageId);
    if (!storage) {
        setError(tr("The note storage is no longer available"));
        return false;
    }
    if (!storage->supportsNativeFolderCatalog()) {
        setError({});
        emit nativeTreePrepared(storageId, true);
        return true;
    }
    startNativeTreePreparation(storage, false);
    return true;
}

bool FolderOperationsController::prepareNativeFolderTrees()
{
    if (!catalogManager_->isAvailable()) {
        setError(catalogManager_->lastError());
        return false;
    }

    for (const auto &storage : noteManager_->storages()) {
        if (!storage || !storage->supportsNativeFolderCatalog())
            continue;
        if (!prepareNativeFolderTree(storage->systemName()))
            return false;
    }
    return true;
}

void FolderOperationsController::setError(const QString &error)
{
    if (!error.isEmpty())
        qCWarning(logFolderOperations) << "Folder operation failed:" << error;
    if (errorString_ == error)
        return;
    errorString_ = error;
    emit errorStringChanged();
}

void FolderOperationsController::beginOperation()
{
    const bool wasBusy = busy();
    ++pendingOperations_;
    if (wasBusy != busy())
        emit busyChanged();
}

void FolderOperationsController::endOperation()
{
    const bool wasBusy = busy();
    pendingOperations_ = qMax(0, pendingOperations_ - 1);
    if (wasBusy != busy())
        emit busyChanged();
}

FolderCatalogResult<FolderCatalogSnapshot> FolderOperationsController::nativeTreeSnapshot(NoteStorage *storage) const
{
    if (!storage)
        return { {}, { FolderCatalogError::NotFound, tr("The note storage is no longer available") } };
    if (!storage->nativeFolderCatalogAvailable()) {
        return { {},
                 { FolderCatalogError::Corrupt,
                   storage->nativeFolderCatalogErrorString().isEmpty() ? tr("The storage folder catalog is unavailable")
                                                                       : storage->nativeFolderCatalogErrorString() } };
    }

    auto       providerSnapshot = storage->nativeFolderCatalog();
    const auto incomingWins     = [](const FolderRecord &incoming, const FolderRecord &current) {
        if (incoming.revision != current.revision)
            return incoming.revision > current.revision;
        if (incoming.modifiedAt.isValid() != current.modifiedAt.isValid())
            return incoming.modifiedAt.isValid();
        return incoming.modifiedAt > current.modifiedAt;
    };
    QSet<QUuid> deletedFolderIds;
    for (const auto &folder : catalogManager_->snapshot().folders) {
        if (!folder.tombstone)
            continue;
        const auto providerFolder
            = std::find_if(providerSnapshot.folders.cbegin(), providerSnapshot.folders.cend(),
                           [&folder](const FolderRecord &candidate) { return candidate.id == folder.id; });
        if (providerFolder == providerSnapshot.folders.cend() || incomingWins(folder, *providerFolder))
            deletedFolderIds.insert(folder.id);
    }
    if (!deletedFolderIds.isEmpty()) {
        providerSnapshot.assignments.erase(
            std::remove_if(providerSnapshot.assignments.begin(), providerSnapshot.assignments.end(),
                           [&deletedFolderIds](const NoteFolderAssignment &assignment) {
                               return !assignment.tombstone && deletedFolderIds.contains(assignment.folderId);
                           }),
            providerSnapshot.assignments.end());
        providerSnapshot.pathHints.erase(std::remove_if(providerSnapshot.pathHints.begin(),
                                                        providerSnapshot.pathHints.end(),
                                                        [&deletedFolderIds](const ProviderPathHint &hint) {
                                                            return deletedFolderIds.contains(hint.folderId);
                                                        }),
                                         providerSnapshot.pathHints.end());
    }

    FolderCatalog providerCatalog;
    if (const auto validation = providerCatalog.replaceSnapshot(std::move(providerSnapshot)))
        return { {}, validation };

    FolderCatalogSnapshot globalFolders;
    globalFolders.folders = catalogManager_->snapshot().folders;
    if (const auto mergeError = providerCatalog.merge(globalFolders))
        return { {}, mergeError };
    return { providerCatalog.snapshot(), {} };
}

bool FolderOperationsController::updateOverlay(const QString &storageId, const QString &noteId, const QUuid &folderId)
{
    const auto result = folderId.isNull() ? catalogManager_->clearNoteAssignment(storageId, noteId)
                                          : catalogManager_->assignNote(storageId, noteId, folderId);
    if (!result) {
        setError({});
        qCInfo(logFolderOperations) << "Folder catalog assignment updated: storage=" << storageId
                                    << "note=" << diagnosticNoteId(noteId)
                                    << "folder=" << folderId.toString(QUuid::WithoutBraces);
        return true;
    }
    setError(result.message);
    return false;
}

void FolderOperationsController::startAssignmentLoad(NoteStorage *storage, const QString &noteId, const QUuid &folderId,
                                                     bool overlayAlreadyStored)
{
    if (!storage) {
        setError(tr("The note storage is no longer available"));
        return;
    }

    const QString               storageId = storage->systemName();
    const QPointer<NoteStorage> storageGuard(storage);
    beginOperation();
    qCInfo(logFolderOperations) << "Resolving note asynchronously for folder assignment: storage=" << storageId
                                << "note=" << diagnosticNoteId(noteId);

    auto *job = storage->loadNoteAsync(noteId, this);
    if (!job) {
        finishAssignment(storageId, noteId, folderId, false,
                         tr("The note storage did not create a note load operation"));
        return;
    }

    const auto handled = QSharedPointer<bool>::create(false);
    const auto finish  = [this, job, handled, storageGuard, storageId, noteId, folderId, overlayAlreadyStored]() {
        if (*handled)
            return;
        *handled = true;
        if (!storageGuard) {
            job->deleteLater();
            finishAssignment(storageId, noteId, folderId, false, tr("The note storage is no longer available"));
            return;
        }
        if (job->state() != StorageJob::Succeeded) {
            const auto jobError = job->error();
            const auto message  = jobError.code == StorageError::NotFound || jobError.message.isEmpty()
                ? tr("The note used for the folder change was not found")
                : jobError.message;
            job->deleteLater();
            finishAssignment(storageId, noteId, folderId, false, message);
            return;
        }

        Note note = job->result();
        job->deleteLater();
        if (note.isNull() || note.id() != noteId) {
            finishAssignment(storageId, noteId, folderId, false,
                             tr("The note used for the folder change was not found"));
            return;
        }
        if (!overlayAlreadyStored && !storeOverlayAssignment(storageId, noteId, folderId)) {
            finishAssignment(storageId, noteId, folderId, false, errorString_);
            return;
        }

        note.setFolderId(folderId);
        startNativeAssignment(storageGuard, note, folderId, true);
    };
    connect(job, &StorageJob::finished, this, finish);
    if (job->isFinished())
        QTimer::singleShot(0, this, finish);
}

void FolderOperationsController::startNativeAssignment(NoteStorage *storage, const Note &note, const QUuid &folderId,
                                                       bool operationAlreadyStarted)
{
    if (!storage) {
        const auto error = tr("The note storage is no longer available");
        setError(error);
        emit assignmentFinished(note.storageId(), note.id(), folderId, false);
        if (operationAlreadyStarted)
            endOperation();
        return;
    }
    if (!operationAlreadyStarted)
        beginOperation();
    qCInfo(logFolderOperations) << "Starting native folder assignment: storage=" << note.storageId()
                                << "note=" << diagnosticNoteId(note.id())
                                << "prepareCatalog=" << storage->supportsNativeFolderCatalog();
    if (storage->supportsNativeFolderCatalog()) {
        startNativeTreePreparation(storage, true, note, folderId);
        return;
    }

    auto *job = storage->changeNoteFolderAsync(note, this);
    if (!job) {
        finishAssignment(note.storageId(), note.id(), folderId, false,
                         tr("The note storage did not create a folder operation"));
        return;
    }
    const auto handled = QSharedPointer<bool>::create(false);
    const auto finish  = [this, job, handled, storageId = note.storageId(), noteId = note.id(), folderId]() {
        if (*handled)
            return;
        *handled             = true;
        const bool succeeded = job->state() == StorageJob::Succeeded;
        const auto error     = succeeded ? QString() : job->error().message;
        job->deleteLater();
        finishAssignment(storageId, noteId, folderId, succeeded, error);
    };
    connect(job, &StorageJob::finished, this, finish);
    if (job->isFinished())
        QTimer::singleShot(0, this, finish);
}

void FolderOperationsController::startNativeTreePreparation(NoteStorage *storage, bool assignmentFollows, Note note,
                                                            QUuid folderId)
{
    if (!storage) {
        if (assignmentFollows)
            finishAssignment(note.storageId(), note.id(), folderId, false,
                             tr("The note storage is no longer available"));
        else {
            setError(tr("The note storage is no longer available"));
            emit nativeTreePrepared({}, false);
        }
        return;
    }

    if (!assignmentFollows)
        beginOperation();
    const auto snapshot = nativeTreeSnapshot(storage);
    if (!snapshot) {
        const auto message = snapshot.error.message;
        if (assignmentFollows)
            finishAssignment(storage->systemName(), note.id(), folderId, false, message);
        else {
            setError(message);
            emit nativeTreePrepared(storage->systemName(), false);
            endOperation();
        }
        return;
    }

    const QString               storageId = storage->systemName();
    const QPointer<NoteStorage> storageGuard(storage);
    auto                       *job = storage->replaceNativeFolderCatalogAsync(snapshot.value, this);
    if (!job) {
        const auto error = tr("The note storage did not create a folder catalog operation");
        if (assignmentFollows)
            finishAssignment(storageId, note.id(), folderId, false, error);
        else {
            setError(error);
            emit nativeTreePrepared(storageId, false);
            endOperation();
        }
        return;
    }
    const auto handled = QSharedPointer<bool>::create(false);
    const auto finish  = [this, job, handled, storageGuard, storageId, assignmentFollows, note, folderId]() {
        if (*handled)
            return;
        *handled = true;
        if (!storageGuard) {
            job->deleteLater();
            const auto error = tr("The note storage is no longer available");
            if (assignmentFollows)
                finishAssignment(storageId, note.id(), folderId, false, error);
            else {
                setError(error);
                emit nativeTreePrepared(storageId, false);
                endOperation();
            }
            return;
        }
        const bool succeeded = job->state() == StorageJob::Succeeded;
        const auto error     = succeeded ? QString() : job->error().message;
        job->deleteLater();
        if (!succeeded) {
            if (assignmentFollows)
                finishAssignment(storageId, note.id(), folderId, false, error);
            else {
                setError(error);
                emit nativeTreePrepared(storageId, false);
                endOperation();
            }
            return;
        }
        if (!assignmentFollows) {
            setError({});
            emit nativeTreePrepared(storageId, true);
            endOperation();
            return;
        }

        auto *change = storageGuard->changeNoteFolderAsync(note, this);
        if (!change) {
            finishAssignment(storageId, note.id(), folderId, false,
                             tr("The note storage did not create a folder operation"));
            return;
        }
        const auto changeHandled = QSharedPointer<bool>::create(false);
        const auto finishChange  = [this, change, changeHandled, storageId, noteId = note.id(), folderId]() {
            if (*changeHandled)
                return;
            *changeHandled     = true;
            const bool changed = change->state() == StorageJob::Succeeded;
            const auto message = changed ? QString() : change->error().message;
            change->deleteLater();
            finishAssignment(storageId, noteId, folderId, changed, message);
        };
        connect(change, &StorageJob::finished, this, finishChange);
        if (change->isFinished())
            QTimer::singleShot(0, this, finishChange);
    };
    connect(job, &StorageJob::finished, this, finish);
    if (job->isFinished())
        QTimer::singleShot(0, this, finish);
}

void FolderOperationsController::finishAssignment(const QString &storageId, const QString &noteId,
                                                  const QUuid &folderId, bool nativeStored, const QString &error)
{
    if (!nativeStored)
        setError(error.isEmpty() ? tr("The folder move is pending") : error);
    else
        setError({});
    qCInfo(logFolderOperations) << "Native folder assignment finished: storage=" << storageId
                                << "note=" << diagnosticNoteId(noteId)
                                << "folder=" << folderId.toString(QUuid::WithoutBraces) << "succeeded=" << nativeStored
                                << "error=" << error;
    emit assignmentFinished(storageId, noteId, folderId, nativeStored);
    endOperation();
}

} // namespace AnyKeep
