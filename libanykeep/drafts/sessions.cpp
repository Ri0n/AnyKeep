#include "draftmanager.h"

#include "private.h"

#include "conflictresolver.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notestorage.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>
#include <QUuid>

#include <utility>

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
    // backendData also contains storage-owned concurrency state, so never
    // replace it wholesale after the draft has been created. Favorite is an
    // AnyKeep user-editable metadata key, though, and must follow every
    // checkpoint just like folderId does.
    if (const auto *storage = note.storage(); storage && storage->supportsFavorite())
        record.backendData.insert(QString::fromLatin1(FavoriteBackendKey), note.isFavorite());
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
    if (!key.isEmpty()) {
        sourceSessions_[key] = id;
        editingSources_[id]  = key;
    }
    qCInfo(logDraftPersistence) << "Acquired editing session: draft=" << id.toString(QUuid::WithoutBraces)
                                << "storage=" << note.storageId() << "noteIdPresent=" << !note.id().isEmpty()
                                << "sessions=" << editingSessions_.value(id);
    return id;
}

void DraftManager::refreshLiveEditorAliases(NoteEditor *editor)
{
    if (!editor)
        return;

    for (auto it = liveEditorsBySource_.begin(); it != liveEditorsBySource_.end();) {
        if (it.value() == editor)
            it = liveEditorsBySource_.erase(it);
        else
            ++it;
    }

    const auto addAlias = [this, editor](const QString &storageId, const QString &noteId) {
        const auto key = sourceKey(storageId, noteId);
        if (!key.isEmpty())
            liveEditorsBySource_[key] = editor;
    };

    addAlias(editor->storageId(), editor->noteId());
    if (!store_)
        return;

    const auto draft = store_->load(editor->draftId());
    if (!draft || draft.value.operation != DraftRecord::Publish)
        return;
    addAlias(draft.value.storageId, draft.value.remoteNoteId);
    addAlias(draft.value.removeSourceStorageId, draft.value.removeSourceNoteId);
}

void DraftManager::removeLiveEditor(NoteEditor *editor)
{
    if (!editor)
        return;
    for (auto it = liveEditorsByDraft_.begin(); it != liveEditorsByDraft_.end();) {
        if (it.value() == editor)
            it = liveEditorsByDraft_.erase(it);
        else
            ++it;
    }
    for (auto it = liveEditorsBySource_.begin(); it != liveEditorsBySource_.end();) {
        if (it.value() == editor)
            it = liveEditorsBySource_.erase(it);
        else
            ++it;
    }
}

NoteEditor *DraftManager::acquireEditor(const Note &note, const QUuid &knownDraftId)
{
    const auto key = sourceKey(note);

    NoteEditor *editor = nullptr;
    if (!knownDraftId.isNull()) {
        // An explicit draft UUID is a stronger identity than the remote
        // storage alias. Distinct recovery/conflict drafts for the same source
        // must never be silently merged into one live document.
        editor = liveEditorsByDraft_.value(knownDraftId);
    } else if (!key.isEmpty()) {
        editor = liveEditorsBySource_.value(key);
        if (!editor) {
            const auto existingDraftId = sourceSessions_.value(key);
            if (!existingDraftId.isNull())
                editor = liveEditorsByDraft_.value(existingDraftId);
        }
    }

    if (editor) {
        // Recovery can create a storage-detached model while a plugin is
        // unavailable. When that same persisted identity later becomes
        // available, attach only its storage/capability context; never reload
        // remote contents over the authoritative live/durable document.
        if (editor->storageId().isEmpty() && !note.storageId().isEmpty() && store_) {
            const auto draft = store_->load(editor->draftId());
            if (draft && draft.value.operation == DraftRecord::Publish && draft.value.storageId == note.storageId()
                && draft.value.remoteNoteId == note.id()) {
                editor->attachStorageContext(note);
            }
        }

        editor->acquireViewLease();
        qCInfo(logDraftPersistence) << "Reusing canonical live editor: draft="
                                    << editor->draftId().toString(QUuid::WithoutBraces)
                                    << "storage=" << editor->storageId() << "noteIdPresent=" << !editor->noteId().isEmpty()
                                    << "views=" << editor->viewLeaseCount();
        return editor;
    }

    editor = new NoteEditor(note, *this, knownDraftId, this);
    liveEditorsByDraft_[editor->draftId()] = editor;
    refreshLiveEditorAliases(editor);

    connect(editor, &NoteEditor::identityChanged, this, [this, editor] { refreshLiveEditorAliases(editor); });
    connect(editor, &NoteEditor::allViewsClosed, this, [this, editor] {
        // Stop new views from acquiring a model whose logical lifecycle ended,
        // but keep the QObject alive until every already-bound QML view has
        // actually detached from it.
        removeLiveEditor(editor);
    });
    connect(editor, &NoteEditor::disposable, this, [editor] { editor->deleteLater(); });
    connect(editor, &QObject::destroyed, this, [this, editor] { removeLiveEditor(editor); });

    qCInfo(logDraftPersistence) << "Registered canonical live editor: draft="
                                << editor->draftId().toString(QUuid::WithoutBraces)
                                << "storage=" << editor->storageId() << "noteIdPresent=" << !editor->noteId().isEmpty();
    return editor;
}

NoteEditor *DraftManager::liveEditorForNote(const QString &storageId, const QString &noteId) const
{
    const auto key = sourceKey(storageId, noteId);
    if (key.isEmpty())
        return nullptr;
    const auto editor = liveEditorsBySource_.value(key);
    if (editor)
        return editor.data();
    const auto draftId = sourceSessions_.value(key);
    return draftId.isNull() ? nullptr : liveEditorsByDraft_.value(draftId);
}

NoteEditor *DraftManager::liveEditorForDraft(const QUuid &draftId) const
{
    return draftId.isNull() ? nullptr : liveEditorsByDraft_.value(draftId);
}

QSet<QUuid> DraftManager::liveDraftIdsForAlias(const QString &storageId, const QString &noteId) const
{
    QSet<QUuid> result;
    const auto  key = sourceKey(storageId, noteId);
    if (key.isEmpty())
        return result;

    if (const auto direct = sourceSessions_.value(key); !direct.isNull())
        result.insert(direct);
    if (const auto editor = liveEditorsBySource_.value(key); editor)
        result.insert(editor->draftId());

    // Detached recovery models and pre-ACK transfers may not expose their
    // durable source through Note::storageId(). The encrypted record is the
    // authoritative alias set, so include both its current persisted identity
    // and its unresolved transfer source.
    if (store_) {
        for (auto it = liveEditorsByDraft_.cbegin(); it != liveEditorsByDraft_.cend(); ++it) {
            auto *editor = it.value().data();
            if (!editor)
                continue;
            const auto draft = store_->load(editor->draftId());
            if (!draft || draft.value.operation != DraftRecord::Publish)
                continue;
            const bool currentMatches
                = draft.value.storageId == storageId && draft.value.remoteNoteId == noteId;
            const bool sourceMatches = draft.value.removeSourceStorageId == storageId
                && draft.value.removeSourceNoteId == noteId;
            if (currentMatches || sourceMatches)
                result.insert(editor->draftId());
        }
    }
    return result;
}

int DraftManager::editingSessionCountForNote(const QString &storageId, const QString &noteId) const
{
    int count = 0;
    for (const auto &draftId : liveDraftIdsForAlias(storageId, noteId))
        count += editingSessions_.value(draftId);
    return count;
}

int DraftManager::editingSessionCount(const QUuid &draftId) const { return editingSessions_.value(draftId); }

bool DraftManager::isLastEditingSession(const QUuid &draftId) const { return editingSessionCount(draftId) <= 1; }

DraftStoreError DraftManager::discardEditingSessionsForNote(const QString &storageId,
                                                                      const QString &noteId)
{
    if (storageId.isEmpty() || noteId.isEmpty())
        return {};

    const auto matchingDrafts = liveDraftIdsForAlias(storageId, noteId);

    // Keep this signal for directly-constructed/test editors which are not in
    // the canonical registry. Registry-owned models are also closed by stable
    // draft UUID below, which covers detached recovery and retargeted views.
    emit discardEditorsForNoteRequested(storageId, noteId);
    for (const auto &draftId : matchingDrafts) {
        if (editingSessionCount(draftId) > 0)
            emit discardEditorsForDraftRequested(draftId);
    }

    for (const auto &draftId : matchingDrafts) {
        if (editingSessionCount(draftId) > 0)
            return { DraftStoreError::Io, tr("Could not close all editors for the note") };
    }
    if (editingSessionCountForNote(storageId, noteId) > 0)
        return { DraftStoreError::Io, tr("Could not close all editors for the note") };
    return {};
}

DraftStoreError DraftManager::discardEditingSessionsForDraft(const QUuid &draftId)
{
    if (draftId.isNull())
        return {};
    emit discardEditorsForDraftRequested(draftId);
    if (editingSessionCount(draftId) > 0)
        return { DraftStoreError::Io, tr("Could not close all editors for the draft") };
    return {};
}

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
    editingSources_.remove(draftId);
    for (auto source = sourceSessions_.begin(); source != sourceSessions_.end();) {
        if (source.value() != draftId) {
            ++source;
            continue;
        }

        const auto key = source.key();
        QUuid      replacement;
        for (auto candidate = editingSources_.cbegin(); candidate != editingSources_.cend(); ++candidate) {
            if (candidate.value() == key && editingSessions_.value(candidate.key()) > 0) {
                replacement = candidate.key();
                break;
            }
        }
        if (replacement.isNull())
            source = sourceSessions_.erase(source);
        else {
            source.value() = replacement;
            ++source;
        }
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
    // Opening a draft must always preserve access to its local contents. A
    // publication can block indefinitely (for example while an upload waits
    // for a remote response), so stop the in-flight job before handing the
    // draft back to an editor. Its completion is ignored because
    // cancelPublication() removes the job from publishJobs_.
    if (draft.value.state == DraftRecord::Publishing)
        cancelPublication(draftId);
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

DraftStoreResult<Note> DraftManager::resumeNoteForEditingDraft(const QUuid &draftId)
{
    const auto draft = resumeEditingDraft(draftId);
    if (!draft)
        return { {}, draft.error };

    Note note;
    if (!draft.value.storageId.isEmpty()) {
        if (auto storage = NoteManager::instance()->storage(draft.value.storageId))
            note = storage->createNote();
    }
    if (note.isNull())
        note = Note(new NoteData(nullptr));

    if (!draft.value.remoteNoteId.isEmpty())
        note.setId(draft.value.remoteNoteId);
    note.setTitle(draft.value.title);
    note.setText(draft.value.body, draft.value.format);
    note.setTags(draft.value.tags);
    note.setFolderId(draft.value.folderId);
    note.setBackendData(draft.value.backendData);
    note.setMedia(draft.value.media);
    return { note, {} };
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

DraftStoreError DraftManager::queueDraftDeletion(const QUuid &draftId)
{
    if (!store_)
        return { DraftStoreError::Locked, lastError_ };
    if (draftId.isNull())
        return { DraftStoreError::InvalidArgument, tr("Draft identifier is empty") };

    const auto pending = store_->load(draftId);
    if (!pending)
        return pending.error;
    if (pending.value.operation != DraftRecord::Publish)
        return { DraftStoreError::InvalidArgument, tr("Only note drafts can be deleted") };

    // Stop an in-flight save before turning its persistent state into delete
    // intents. queueRemoval() is synchronous; its publishPending() calls are
    // queued, so no remote callback can observe a half-converted lifecycle in
    // this event-loop turn.
    cancelPublication(draftId);

    QList<QPair<QString, QString>> objects;
    const auto appendObject = [&objects](const QString &storageId, const QString &noteId) {
        if (storageId.isEmpty() || noteId.isEmpty())
            return;
        const QPair<QString, QString> object { storageId, noteId };
        if (!objects.contains(object))
            objects.append(object);
    };

    // After destination acknowledgement but before source cleanup is durable,
    // both identities can exist. Explicit delete owns both; forgetting either
    // one leaves a ghost duplicate after the transfer draft is discarded.
    appendObject(pending.value.storageId, pending.value.remoteNoteId);
    appendObject(pending.value.removeSourceStorageId, pending.value.removeSourceNoteId);

    for (const auto &object : std::as_const(objects)) {
        if (const auto error = queueRemoval(object.first, object.second))
            return error; // Keep the publish draft as the reconciliation root.
    }

    auto removeError = store_->remove(draftId);
    if (removeError.code == DraftStoreError::NotFound)
        removeError = {};
    if (removeError)
        return removeError;

    emit draftsChanged();
    QTimer::singleShot(0, this, &DraftManager::publishPending);
    return {};
}

DraftStoreResult<QPair<QString, QString>>
DraftManager::prepareForRecycle(const QString &storageId, const QString &noteId, const QUuid &knownDraftId)
{
    if (!store_)
        return { {}, { DraftStoreError::Locked, lastError_ } };

    QUuid draftId = knownDraftId;
    DraftStoreResult<DraftRecord> pending { {}, { DraftStoreError::NotFound, {} } };

    if (!draftId.isNull()) {
        pending = pendingDraft(draftId);
        if (!pending && pending.error.code != DraftStoreError::NotFound)
            return { {}, pending.error };
    } else if (storageId == draftsStorageId()) {
        draftId = QUuid(noteId);
        if (draftId.isNull())
            return { {}, { DraftStoreError::InvalidArgument, tr("The draft identifier is invalid") } };
        pending = pendingDraft(draftId);
        if (!pending)
            return { {}, pending.error };
    } else if (!storageId.isEmpty() && !noteId.isEmpty()) {
        pending = pendingDraftForNote(storageId, noteId);
        if (!pending && pending.error.code != DraftStoreError::NotFound)
            return { {}, pending.error };

        if (!pending) {
            const QUuid presentedId(noteId);
            if (!presentedId.isNull()) {
                auto presented = pendingDraft(presentedId);
                if (presented && presented.value.storageId == storageId) {
                    pending = std::move(presented);
                    draftId = presentedId;
                }
            }
        } else {
            draftId = pending.value.id;
        }
    }

    if (!pending) {
        // A known live draft UUID can legitimately have no persisted record
        // yet when the note has never changed.
        const auto closeError = !draftId.isNull() ? discardEditingSessionsForDraft(draftId)
                                                  : discardEditingSessionsForNote(storageId, noteId);
        if (closeError)
            return { {}, closeError };
        if (storageId.isEmpty() || noteId.isEmpty() || storageId == draftsStorageId())
            return { {}, {} };
        return { { storageId, noteId }, {} };
    }

    const auto record = pending.value;
    QPair<QString, QString> recycleTarget;
    if (!record.remoteNoteId.isEmpty()) {
        recycleTarget = { record.storageId, record.remoteNoteId };
    } else if (!record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty()) {
        recycleTarget = { record.removeSourceStorageId, record.removeSourceNoteId };
    }

    if (const auto closeError = discardEditingSessionsForDraft(record.id))
        return { {}, closeError };

    const bool destinationAcknowledged = !record.remoteNoteId.isEmpty();
    const bool sourceStillPending = !record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty()
        && (record.removeSourceStorageId != record.storageId || record.removeSourceNoteId != record.remoteNoteId);
    if (destinationAcknowledged && sourceStillPending) {
        if (const auto removalError = queueRemoval(record.removeSourceStorageId, record.removeSourceNoteId))
            return { {}, removalError };
    }

    if (const auto discardError = discard(record.id))
        return { {}, discardError };

    return { recycleTarget, {} };
}

} // namespace AnyKeep
