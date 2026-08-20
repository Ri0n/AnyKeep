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
                                            const QUuid &destinationFolderId, QUuid *draftId)
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

    const QString title = NoteTransferController::convertTextFormat(source.title(), source.format(), destinationFormat);
    const QString body  = NoteTransferController::convertTextFormat(source.text(), source.format(), destinationFormat);
    destination.setTitle(title);
    destination.setText(body, destinationFormat);
    destination.setTags(source.tags());
    destination.setFolderId(destinationFolderId);
    destination.setMedia(source.media());

    const QUuid transferDraftId = acquireEditingSession(destination);
    const auto  saveError       = saveEditing(transferDraftId, destination, title, body, destinationFormat);
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

    Note::Format targetFormat = record->format;
    if (const auto formatError = resolveDestinationFormat(destinationStorage, record->format, &targetFormat))
        return formatError;

    const bool hasPublishedSource = !record->remoteNoteId.isEmpty() && record->removeSourceStorageId.isEmpty();
    if (hasPublishedSource && record->storageId.isEmpty()) {
        return { DraftStoreError::InvalidArgument, tr("The draft source storage is missing") };
    }
    if (targetFormat != record->format) {
        record->title  = NoteTransferController::convertTextFormat(record->title, record->format, targetFormat);
        record->body   = NoteTransferController::convertTextFormat(record->body, record->format, targetFormat);
        record->format = targetFormat;
    }
    if (hasPublishedSource) {
        record->removeSourceStorageId = record->storageId;
        record->removeSourceNoteId    = record->remoteNoteId;
    }
    record->storageId = destinationId;
    record->remoteNoteId.clear();
    record->backendData.clear();
    record->state = DraftRecord::Ready;
    record->lastError.clear();
    record->retryAt = {};
    return {};
}

} // namespace AnyKeep
