#include "draftmanager.h"

#include "private.h"

#include "conflictresolver.h"
#include "notedata.h"
#include "notestorage.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <QUuid>

// Uncomment for detailed draft publication/conflict diagnostics.
// #define ANYKEEP_ENABLE_CONFLICT_TRACE

#ifdef ANYKEEP_ENABLE_CONFLICT_TRACE
#define CONFLICT_TRACE qInfo().noquote()
#else
#define CONFLICT_TRACE QNoDebug()
#endif

namespace AnyKeep {

using DraftManagerPrivate::concurrencySummary;
using DraftManagerPrivate::draftStateName;

DraftStoreError DraftManager::saveEditing(const QUuid &draftId, const Note &note, const QString &title,
                                          const QString &body, Note::Format format, bool folderUserOverride)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ };
    auto        existing = store_->load(draftId);
    DraftRecord record   = existing ? existing.value : DraftRecord {};
    record.id            = draftId;
    record.state         = DraftRecord::Editing;
    if (!existing) {
        record.storageId    = note.storageId();
        record.remoteNoteId = note.id();
        record.backendData  = note.backendData();
    }
    record.title              = title;
    record.body               = body;
    record.format             = format;
    record.tags               = NoteData::tagsFromText(body);
    record.folderId           = note.folderId();
    record.folderUserOverride = existing ? existing.value.folderUserOverride || folderUserOverride : folderUserOverride;
    record.media              = note.media();
    record.revision           = existing ? existing.value.revision + 1 : 1;
    record.updatedAt          = QDateTime::currentDateTimeUtc();
    CONFLICT_TRACE << "Conflict trace: draft captured id=" << draftId.toString(QUuid::WithoutBraces)
                   << "storage=" << record.storageId << "note=" << record.remoteNoteId
                   << "base=" << concurrencySummary(record.backendData);
    qCInfo(logDraftPersistence) << "Saving editing draft: id=" << draftId.toString(QUuid::WithoutBraces)
                                << "storage=" << record.storageId
                                << "remoteNotePresent=" << !record.remoteNoteId.isEmpty()
                                << "revision=" << record.revision << "titleLength=" << title.size()
                                << "bodyLength=" << body.size() << "format=" << int(format);
    const auto result = store_->write(record);
    if (result)
        qCWarning(logDraftPersistence) << "Failed to save editing draft" << draftId.toString(QUuid::WithoutBraces)
                                       << int(result.code) << result.message;
    else
        qCInfo(logDraftPersistence) << "Editing draft saved" << draftId.toString(QUuid::WithoutBraces);
    if (!result)
        emit draftsChanged();
    return result;
}

QUuid DraftManager::acquireEditingSession(const Note &note, const QUuid &knownDraftId)
{
    QUuid      id  = knownDraftId;
    const auto key = sourceKey(note);
    if (id.isNull() && !key.isEmpty())
        id = sourceSessions_.value(key);
    if (id.isNull() && store_ && !key.isEmpty()) {
        const auto records = store_->records();
        if (records) {
            QDateTime latest;
            for (const auto &record : records.value) {
                if (record.operation != DraftRecord::Publish || record.state != DraftRecord::Editing
                    || record.storageId != note.storageId() || record.remoteNoteId != note.id())
                    continue;
                if (id.isNull() || record.updatedAt > latest) {
                    id     = record.id;
                    latest = record.updatedAt;
                }
            }
        }
    }
    if (id.isNull())
        id = QUuid::createUuid();
    ++editingSessions_[id];
    if (!key.isEmpty())
        sourceSessions_[key] = id;
    qCInfo(logDraftPersistence) << "Acquired editing session: draft=" << id.toString(QUuid::WithoutBraces)
                                << "storage=" << note.storageId() << "noteIdPresent=" << !note.id().isEmpty()
                                << "sessions=" << editingSessions_.value(id);
    return id;
}

bool DraftManager::isLastEditingSession(const QUuid &draftId) const { return editingSessions_.value(draftId, 1) <= 1; }

bool DraftManager::releaseEditingSession(const QUuid &draftId)
{
    auto it = editingSessions_.find(draftId);
    if (it == editingSessions_.end()) {
        qCInfo(logDraftPersistence) << "Release requested for unknown editing session"
                                    << draftId.toString(QUuid::WithoutBraces);
        return true;
    }
    if (--it.value() > 0) {
        qCInfo(logDraftPersistence) << "Editing session still shared: draft=" << draftId.toString(QUuid::WithoutBraces)
                                    << "sessions=" << it.value();
        return false;
    }
    editingSessions_.erase(it);
    for (auto source = sourceSessions_.begin(); source != sourceSessions_.end();) {
        if (source.value() == draftId)
            source = sourceSessions_.erase(source);
        else
            ++source;
    }
    qCInfo(logDraftPersistence) << "Released final editing session" << draftId.toString(QUuid::WithoutBraces);
    return true;
}

DraftStoreResult<DraftRecord> DraftManager::editingDraft(const QUuid &draftId) const
{
    if (!store_)
        return { {}, { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ } };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft;
    if (draft.value.operation != DraftRecord::Publish || draft.value.state != DraftRecord::Editing)
        return { {}, { DraftStoreError::NotFound, tr("No active editing draft was found") } };
    return draft;
}

DraftStoreResult<DraftRecord> DraftManager::resumeEditingDraft(const QUuid &draftId)
{
    if (!store_)
        return { {}, { DraftStoreError::Locked, lastError_.isEmpty() ? tr("Draft store is locked") : lastError_ } };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft;
    if (draft.value.operation != DraftRecord::Publish)
        return { {}, { DraftStoreError::InvalidArgument, tr("Only publish drafts can be resumed for editing") } };
    if (draft.value.state == DraftRecord::Publishing)
        return { {}, { DraftStoreError::Locked, tr("The draft is already being published") } };
    if (draft.value.state == DraftRecord::Editing)
        return draft;

    draft.value.state     = DraftRecord::Editing;
    draft.value.lastError = {};
    draft.value.retryAt   = {};
    const auto error      = store_->write(draft.value);
    if (error)
        return { {}, error };
    emit draftsChanged();
    qCInfo(logDraftPersistence) << "Resumed draft for update-session editing:"
                                << draftId.toString(QUuid::WithoutBraces);
    return draft;
}

void DraftManager::setConflictResolver(std::unique_ptr<ConflictResolver> resolver)
{
    conflictResolver_ = resolver ? std::move(resolver) : std::make_unique<CopyConflictResolver>();
}

void DraftManager::resolveConcurrentEdit(const Note &localVersion, const Note &remoteVersion, const QString &message)
{
    if (!store_ || localVersion.isNull() || localVersion.storageId().isEmpty())
        return;

    DraftRecord record;
    record.id           = QUuid::createUuid();
    record.state        = DraftRecord::Editing;
    record.storageId    = localVersion.storageId();
    record.remoteNoteId = localVersion.id();
    record.title        = localVersion.title();
    record.body         = localVersion.text();
    record.format       = localVersion.format();
    record.tags         = localVersion.tags();
    record.backendData  = localVersion.backendData();
    record.media        = localVersion.media();
    record.updatedAt    = QDateTime::currentDateTimeUtc();
    record.lastError    = message;
    CONFLICT_TRACE << "Conflict trace: post-publication conflict note=" << record.remoteNoteId
                   << "local=" << concurrencySummary(record.backendData)
                   << "remote=" << concurrencySummary(remoteVersion.backendData());
    if (const auto writeError = store_->write(record)) {
        emit publicationAbandoned(tr("Failed to preserve a conflicting note: %1").arg(writeError.message));
        return;
    }

    StorageError error { StorageError::Conflict, message, false };
    resolveConflict(record, error, remoteVersion);
}

DraftStoreError DraftManager::markReady(const QUuid &draftId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_ };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft.error;
    draft.value.state = draft.value.storageId.isEmpty() ? DraftRecord::NeedsRouting : DraftRecord::Ready;
    CONFLICT_TRACE << "Conflict trace: draft ready id=" << draftId.toString(QUuid::WithoutBraces)
                   << "note=" << draft.value.remoteNoteId << "base=" << concurrencySummary(draft.value.backendData);
    qCInfo(logDraftPersistence) << "Marking draft ready: id=" << draftId.toString(QUuid::WithoutBraces)
                                << "state=" << draftStateName(draft.value.state) << "storage=" << draft.value.storageId
                                << "remoteNotePresent=" << !draft.value.remoteNoteId.isEmpty();
    auto result = store_->write(draft.value);
    if (result)
        qCWarning(logDraftPersistence) << "Failed to mark draft ready" << draftId.toString(QUuid::WithoutBraces)
                                       << int(result.code) << result.message;
    if (!result) {
        emit draftsChanged();
        QTimer::singleShot(0, this, &DraftManager::publishPending);
    }
    return result;
}

DraftStoreError DraftManager::discard(const QUuid &draftId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_ };
    cancelPublication(draftId);
    auto result = store_->remove(draftId);
    if (result.code == DraftStoreError::NotFound)
        result = {};
    if (!result)
        emit draftsChanged();
    return result;
}

DraftStoreError DraftManager::setDraftFolder(const QUuid &draftId, const QUuid &folderId, bool userOverride)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_ };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft.error;
    if (draft.value.operation != DraftRecord::Publish)
        return { DraftStoreError::InvalidArgument, tr("Only note drafts can be assigned to folders") };

    draft.value.folderId           = folderId;
    draft.value.folderUserOverride = draft.value.folderUserOverride || userOverride;
    draft.value.updatedAt          = QDateTime::currentDateTimeUtc();
    const auto error               = store_->write(draft.value);
    if (!error)
        emit draftsChanged();
    return error;
}

DraftStoreError DraftManager::retryDraftNow(const QUuid &draftId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_ };
    auto draft = store_->load(draftId);
    if (!draft)
        return draft.error;
    if (draft.value.operation != DraftRecord::Publish)
        return { DraftStoreError::InvalidArgument, tr("Only note drafts can be published") };

    cancelPublication(draftId);
    draft.value.state = draft.value.storageId.isEmpty() ? DraftRecord::NeedsRouting : DraftRecord::Ready;
    draft.value.lastError.clear();
    draft.value.retryAt   = {};
    draft.value.updatedAt = QDateTime::currentDateTimeUtc();
    const auto error      = store_->write(draft.value);
    if (!error) {
        emit draftsChanged();
        QTimer::singleShot(0, this, &DraftManager::publishPending);
    }
    return error;
}

DraftStoreError DraftManager::queueRemoval(const QString &storageId, const QString &noteId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_ };
    if (storageId.isEmpty() || noteId.isEmpty())
        return { DraftStoreError::InvalidArgument, tr("Storage or note identifier is empty") };

    auto records = store_->records();
    if (!records)
        return records.error;
    for (const auto &record : records.value) {
        if (record.operation == DraftRecord::Delete && record.storageId == storageId && record.remoteNoteId == noteId) {
            return {};
        }
    }

    DraftRecord record;
    record.id           = QUuid::createUuid();
    record.operation    = DraftRecord::Delete;
    record.state        = DraftRecord::Ready;
    record.storageId    = storageId;
    record.remoteNoteId = noteId;
    record.updatedAt    = QDateTime::currentDateTimeUtc();
    auto result         = store_->write(record);
    if (!result)
        QTimer::singleShot(0, this, &DraftManager::publishPending);
    return result;
}

} // namespace AnyKeep
