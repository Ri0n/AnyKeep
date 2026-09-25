#include "draftmanager.h"

#include "notemanager.h"
#include "notestorage.h"
#include "notetransfercontroller.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>

namespace AnyKeep {
namespace {

    DraftStoreError resolveDestinationFormat(const NoteStorage *storage, Note::Format sourceFormat,
                                             Note::Format *destinationFormat)
    {
        if (!storage || !destinationFormat)
            return { DraftStoreError::InvalidArgument, QStringLiteral("A destination storage is required") };
        const auto formats = storage->availableFormats();
        if (formats.contains(sourceFormat)) {
            *destinationFormat = sourceFormat;
            return {};
        }
        const QList<Note::Format> conversionPreference {
            Note::Markdown,
            Note::PlainText,
            Note::Html,
        };
        const auto supported = std::ranges::find_if(
            conversionPreference, [&formats](Note::Format format) { return formats.contains(format); });
        if (supported == conversionPreference.cend()) {
            return { DraftStoreError::InvalidArgument,
                     QStringLiteral("The destination storage does not support this note format") };
        }
        *destinationFormat = *supported;
        return {};
    }

} // namespace

DraftStoreError DraftManager::stageTransfer(const Note &source, const QString &destinationStorageId,
                                            const QUuid &destinationFolderId, QUuid *draftId,
                                            bool folderUserOverride)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ };
    if (draftId)
        *draftId = {};
    if (source.isNull() || source.storageId().isEmpty() || source.id().isEmpty() || destinationStorageId.isEmpty()) {
        return { DraftStoreError::InvalidArgument, tr("A source note and destination storage are required") };
    }
    if (source.storageId() == destinationStorageId)
        return { DraftStoreError::InvalidArgument, tr("The source and destination storage are the same") };

    auto destinationStorage = NoteManager::instance()->storage(destinationStorageId);
    if (!destinationStorage || !destinationStorage->canAcceptWrites())
        return { DraftStoreError::Io, tr("The destination storage is unavailable") };

    Note::Format destinationFormat = source.format();
    if (const auto formatError = resolveDestinationFormat(destinationStorage, source.format(), &destinationFormat))
        return formatError;
    if (!source.media().isEmpty() && !destinationStorage->supportsMedia()) {
        return { DraftStoreError::InvalidArgument, tr("The destination storage does not support note attachments") };
    }

    Note destination = destinationStorage->createNote();
    if (destination.isNull())
        return { DraftStoreError::Io, tr("Could not create the destination note") };

    // The durable draft stores the canonical logical document. Conversion to
    // a storage-specific representation is a publication-boundary concern, so
    // retargeting before acknowledgement is lossless and reversible.
    const QString title = source.title();
    const QString body  = source.text();
    destination.setTitle(title);
    destination.setText(body, source.format());
    destination.setTags(source.tags());
    destination.setFolderId(destinationFolderId);
    destination.setMedia(source.media());
    if (destinationStorage->supportsFavorite())
        destination.setFavorite(source.isFavorite());

    const QUuid transferDraftId = acquireEditingSession(destination);
    const auto  saveError
        = saveEditing(transferDraftId, destination, title, body, source.format(), folderUserOverride);
    if (saveError) {
        releaseEditingSession(transferDraftId);
        return saveError;
    }

    auto transfer = store_->load(transferDraftId);
    if (!transfer) {
        releaseEditingSession(transferDraftId);
        return transfer.error;
    }
    transfer.value.tags                  = source.tags();
    transfer.value.removeSourceStorageId = source.storageId();
    transfer.value.removeSourceNoteId    = source.id();
    transfer.value.updatedAt             = QDateTime::currentDateTimeUtc();
    if (const auto writeError = store_->write(transfer.value)) {
        releaseEditingSession(transferDraftId);
        return writeError;
    }

    const auto readyError = markReady(transferDraftId);
    releaseEditingSession(transferDraftId);
    if (readyError)
        return readyError;
    if (draftId)
        *draftId = transferDraftId;
    return {};
}

DraftStoreResult<DraftRecord> DraftManager::retargetEditingDraft(const QUuid &draftId,
                                                                              const QString &destinationStorageId)
{
    if (!store_)
        return { {}, { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ } };

    auto draft = store_->load(draftId);
    if (!draft)
        return draft;
    if (draft.value.operation != DraftRecord::Publish || draft.value.state != DraftRecord::Editing) {
        return { {}, { DraftStoreError::InvalidArgument, tr("Only a live editing draft can change storage") } };
    }

    if (const auto error = retargetDraftForPublication(&draft.value, destinationStorageId))
        return { {}, error };

    // retargetDraftForPublication() prepares a publishable record. A live
    // shared document must stay Editing until its last view closes.
    draft.value.state     = DraftRecord::Editing;
    draft.value.updatedAt = QDateTime::currentDateTimeUtc();
    ++draft.value.revision;
    if (const auto error = store_->write(draft.value))
        return { {}, error };

    emit draftsChanged();
    return draft;
}

DraftStoreError DraftManager::moveDraft(const QUuid &draftId, const QString &destinationStorageId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft.error;
    if (draft.value.operation != DraftRecord::Publish)
        return { DraftStoreError::InvalidArgument, tr("Only note drafts can be moved between storages") };

    cancelPublication(draftId);
    if (const auto error = retargetDraftForPublication(&draft.value, destinationStorageId))
        return error;
    draft.value.updatedAt = QDateTime::currentDateTimeUtc();
    if (const auto error = store_->write(draft.value))
        return error;
    emit draftsChanged();
    QTimer::singleShot(0, this, &DraftManager::publishPending);
    return {};
}

DraftStoreError DraftManager::copyDraft(const QUuid &draftId, const QString &destinationStorageId, QUuid *copyDraftId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ };
    if (copyDraftId)
        *copyDraftId = {};
    const auto source = store_->load(draftId);
    if (!source)
        return source.error;
    if (source.value.operation != DraftRecord::Publish)
        return { DraftStoreError::InvalidArgument, tr("Only note drafts can be copied between storages") };

    DraftRecord copy = source.value;
    copy.id          = QUuid::createUuid();
    copy.removeSourceStorageId.clear();
    copy.removeSourceNoteId.clear();
    copy.remoteNoteId.clear();
    copy.backendData.clear();
    copy.state = DraftRecord::Ready;
    copy.lastError.clear();
    copy.retryAt  = {};
    copy.revision = 1;
    if (const auto error = retargetDraftForPublication(&copy, destinationStorageId))
        return error;
    copy.updatedAt = QDateTime::currentDateTimeUtc();
    if (const auto error = store_->write(copy))
        return error;
    if (copyDraftId)
        *copyDraftId = copy.id;
    emit draftsChanged();
    QTimer::singleShot(0, this, &DraftManager::publishPending);
    return {};
}

bool DraftManager::hasPendingTransferFrom(const QString &storageId, const QString &noteId) const
{
    if (!store_ || storageId.isEmpty() || noteId.isEmpty())
        return false;
    const auto records = store_->records();
    if (!records)
        return false;
    return std::any_of(records.value.cbegin(), records.value.cend(), [&storageId, &noteId](const DraftRecord &record) {
        return record.operation == DraftRecord::Publish && record.removeSourceStorageId == storageId
            && record.removeSourceNoteId == noteId;
    });
}

void DraftManager::setPrePublicationHandler(PrePublicationHandler handler)
{
    prePublicationHandler_ = std::move(handler);
}

DraftStoreError DraftManager::retargetDraftForPublication(DraftRecord   *record,
                                                          const QString &destinationStorageId) const
{
    if (!record)
        return { DraftStoreError::InvalidArgument, tr("A draft is required") };
    const auto destinationId = destinationStorageId.trimmed();
    if (destinationId.isEmpty())
        return { DraftStoreError::InvalidArgument, tr("A destination storage is required") };

    const auto destinationStorage = NoteManager::instance()->storage(destinationId);
    if (!destinationStorage || !destinationStorage->canAcceptWrites())
        return { DraftStoreError::Io, tr("The destination storage is unavailable") };
    if (!record->media.isEmpty() && !destinationStorage->supportsMedia()) {
        return { DraftStoreError::InvalidArgument, tr("The destination storage does not support note attachments") };
    }
    if (record->removeSourceStorageId.isEmpty() != record->removeSourceNoteId.isEmpty()) {
        return { DraftStoreError::InvalidArgument, tr("The draft transfer source is incomplete") };
    }

    // Once a destination has been acknowledged, the remaining durable work is
    // source deletion. Rerouting it would create another copy and make that
    // acknowledgement ambiguous.
    if (!record->removeSourceStorageId.isEmpty() && !record->remoteNoteId.isEmpty()) {
        if (record->storageId != destinationId) {
            return { DraftStoreError::InvalidArgument,
                     tr("The transfer destination was already published and cannot be changed") };
        }
        return {};
    }

    if (record->storageId == destinationId) {
        if (record->state == DraftRecord::NeedsRouting) {
            record->state = DraftRecord::Ready;
            record->lastError.clear();
            record->retryAt = {};
        }
        return {};
    }

    // Validate that publication can represent this canonical format, but do
    // not convert the draft itself. The live/durable document is storage
    // independent; conversion happens only when a Note is submitted.
    Note::Format targetFormat = record->format;
    if (const auto formatError = resolveDestinationFormat(destinationStorage, record->format, &targetFormat))
        return formatError;

    const bool transferPending
        = !record->removeSourceStorageId.isEmpty() && !record->removeSourceNoteId.isEmpty();

    // Before destination acknowledgement, moving back to the persisted source
    // cancels the transfer. Restore the original remote identity and its base
    // concurrency token instead of creating a new source note and deleting the
    // original afterwards.
    if (transferPending && record->remoteNoteId.isEmpty() && destinationId == record->removeSourceStorageId) {
        record->storageId    = record->removeSourceStorageId;
        record->remoteNoteId = record->removeSourceNoteId;
        record->removeSourceStorageId.clear();
        record->removeSourceNoteId.clear();
        record->state = DraftRecord::Ready;
        record->lastError.clear();
        record->retryAt = {};
        return {};
    }

    const bool hasPublishedSource = !record->remoteNoteId.isEmpty() && !transferPending;
    if (hasPublishedSource && record->storageId.isEmpty())
        return { DraftStoreError::InvalidArgument, tr("The draft source storage is missing") };

    if (hasPublishedSource) {
        record->removeSourceStorageId = record->storageId;
        record->removeSourceNoteId    = record->remoteNoteId;
        // Keep backendData: it is the original source concurrency token and is
        // required if the user retargets back before destination ACK. It is
        // deliberately not sent wholesale to a different destination.
    } else if (!transferPending) {
        // An unpublished note has no source token worth preserving. Drop
        // target-specific hints when rerouting, but keep portable user
        // metadata which belongs to the logical note rather than a backend.
        QVariantMap portableData;
        const auto favoriteKey = QString::fromLatin1(FavoriteBackendKey);
        if (record->backendData.contains(favoriteKey))
            portableData.insert(favoriteKey, record->backendData.value(favoriteKey));
        record->backendData = std::move(portableData);
    }

    record->storageId = destinationId;
    record->remoteNoteId.clear();
    record->state = DraftRecord::Ready;
    record->lastError.clear();
    record->retryAt = {};
    return {};
}

} // namespace AnyKeep
