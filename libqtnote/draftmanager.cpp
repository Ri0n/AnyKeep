#include "draftmanager.h"

#include "conflictresolver.h"

#include "filedraftstore.h"
#include "localdatakeystore.h"
#include "notedata.h"
#include "notemanager.h"
#include "notestorage.h"
#include "notetransfercontroller.h"
#include "storagejob.h"
#include "utils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTimer>
#include <QUuid>

// Uncomment for detailed draft publication/conflict diagnostics.
// #define QTNOTE_ENABLE_CONFLICT_TRACE

#ifdef QTNOTE_ENABLE_CONFLICT_TRACE
#define CONFLICT_TRACE qInfo().noquote()
#else
#define CONFLICT_TRACE QNoDebug()
#endif
namespace QtNote {

Q_LOGGING_CATEGORY(logDraftPersistence, "qtnote.persistence.drafts")

namespace {

    const char *draftStateName(DraftRecord::State state)
    {
        switch (state) {
        case DraftRecord::Editing:
            return "editing";
        case DraftRecord::Ready:
            return "ready";
        case DraftRecord::Publishing:
            return "publishing";
        case DraftRecord::Retry:
            return "retry";
        case DraftRecord::NeedsRouting:
            return "needs-routing";
        }
        return "unknown";
    }

    const char *draftOperationName(DraftRecord::Operation operation)
    {
        return operation == DraftRecord::Delete ? "delete" : "publish";
    }

    QString concurrencySummary(const QVariantMap &data)
    {
        QStringList       parts;
        const QStringList keys { QStringLiteral("revision"), QStringLiteral("etag"), QStringLiteral("keep.baseVersion"),
                                 QStringLiteral("originId") };
        for (const auto &key : keys) {
            if (!data.contains(key))
                continue;
            auto value = data.value(key).toString();
            if (value.isEmpty())
                value = QStringLiteral("<set>");
            parts.append(key + QLatin1Char('=') + value);
        }
        return parts.isEmpty() ? QStringLiteral("<none>") : parts.join(QLatin1Char(' '));
    }

    bool sameMediaReference(const MediaReference &left, const MediaReference &right)
    {
        return left.id == right.id && left.blobId == right.blobId && left.originalName == right.originalName
            && left.portableName == right.portableName && left.mediaType == right.mediaType && left.size == right.size
            && left.checksum == right.checksum && left.remoteData == right.remoteData;
    }

    bool hasSamePublishedContents(const DraftRecord &draft, const Note &note)
    {
        if (draft.title != note.title() || draft.body != note.text() || draft.format != note.format()
            || draft.folderId != note.folderId() || draft.media.size() != note.media().size())
            return false;
        const auto media = note.media();
        for (qsizetype i = 0; i < draft.media.size(); ++i) {
            if (!sameMediaReference(draft.media.at(i), media.at(i)))
                return false;
        }
        return true;
    }

    bool sameDraftRecord(const DraftRecord &left, const DraftRecord &right)
    {
        if (left.id != right.id || left.operation != right.operation || left.state != right.state
            || left.storageId != right.storageId || left.remoteNoteId != right.remoteNoteId || left.title != right.title
            || left.body != right.body || left.format != right.format || left.tags != right.tags
            || left.folderId != right.folderId || left.folderUserOverride != right.folderUserOverride
            || left.removeSourceStorageId != right.removeSourceStorageId
            || left.removeSourceNoteId != right.removeSourceNoteId || left.backendData != right.backendData
            || left.revision != right.revision || left.updatedAt != right.updatedAt || left.lastError != right.lastError
            || left.retryAt != right.retryAt || left.media.size() != right.media.size()) {
            return false;
        }
        for (qsizetype index = 0; index < left.media.size(); ++index) {
            if (!sameMediaReference(left.media.at(index), right.media.at(index)))
                return false;
        }
        return true;
    }

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

DraftManager::DraftManager(QObject *parent) :
    QObject(parent), conflictResolver_(std::make_unique<CopyConflictResolver>())
{
}

DraftManager::DraftManager(std::unique_ptr<DraftStore> store, QObject *parent) :
    QObject(parent), store_(std::move(store)), conflictResolver_(std::make_unique<CopyConflictResolver>())
{
}
DraftManager::~DraftManager() = default;

QString DraftManager::sourceKey(const Note &note)
{
    if (note.storageId().isEmpty() || note.id().isEmpty())
        return {};
    return note.storageId() + QChar(0x1f) + note.id();
}

DraftManager *DraftManager::instance()
{
    static DraftManager *manager = new DraftManager(QCoreApplication::instance());
    return manager;
}

bool DraftManager::initialize(QString *errorText)
{
    if (store_) {
        qCInfo(logDraftPersistence) << "Draft manager already initialized";
        return true;
    }
    QString error;
    auto    key = LocalDataKeyStore::loadOrCreateMasterKey(&error);
    if (key.isEmpty()) {
        lastError_ = error;
        if (errorText)
            *errorText = error;
        return false;
    }
    const QString draftsPath = Utils::qtnoteDataDir() + QStringLiteral("/drafts");
    qCInfo(logDraftPersistence) << "Initializing draft store at" << draftsPath;
    store_       = std::make_unique<FileDraftStore>(draftsPath, std::move(key));
    auto records = store_->records();
    if (!records) {
        lastError_ = records.error.message;
        store_.reset();
        if (errorText)
            *errorText = lastError_;
        return false;
    }
    qCInfo(logDraftPersistence) << "Draft store initialized with" << records.value.size() << "records";
    for (const auto &record : records.value) {
        qCInfo(logDraftPersistence) << "Existing draft: id=" << record.id.toString(QUuid::WithoutBraces)
                                    << "operation=" << draftOperationName(record.operation)
                                    << "state=" << draftStateName(record.state) << "storage=" << record.storageId
                                    << "remoteNotePresent=" << !record.remoteNoteId.isEmpty()
                                    << "revision=" << record.revision << "titleLength=" << record.title.size()
                                    << "bodyLength=" << record.body.size() << "lastError=" << record.lastError;
    }
    auto *notes = NoteManager::instance();
    connect(notes, &NoteManager::storageAboutToBeRemoved, this,
            [this](NoteStorage::Ptr storage) { storageAboutToBeRemoved(storage.data()); });
    connect(notes, &NoteManager::storageRemoved, this,
            [this](NoteStorage::Ptr) { QTimer::singleShot(0, this, &DraftManager::publishPending); });
    connect(notes, &NoteManager::storageReady, this,
            [this](NoteStorage::Ptr storage) { storageBecameReady(storage.data()); });
    QTimer::singleShot(0, this, &DraftManager::publishPending);
    return true;
}

bool DraftManager::recreateStore(QString *errorText)
{
    if (store_)
        return true;

    const QString   draftsPath = Utils::qtnoteDataDir() + QStringLiteral("/drafts");
    const QFileInfo draftsInfo(draftsPath);
    if (draftsInfo.exists()) {
        const QString backupName
            = QStringLiteral("drafts-unrecoverable-%1-%2")
                  .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")),
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
        QDir parent(draftsInfo.absolutePath());
        if (!parent.rename(draftsInfo.fileName(), backupName)) {
            lastError_ = tr("Failed to preserve the unreadable draft store for recovery.");
            if (errorText)
                *errorText = lastError_;
            return false;
        }
        qCWarning(logDraftPersistence) << "Moved unreadable draft store to" << parent.filePath(backupName);
    }

    lastError_.clear();
    return initialize(errorText);
}

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
    auto result = store_->remove(draftId);
    if (result.code == DraftStoreError::NotFound)
        result = {};
    if (!result)
        emit draftsChanged();
    return result;
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

QList<DraftRecord> DraftManager::recoverableDrafts() const
{
    QList<DraftRecord> result;
    if (!store_)
        return result;
    auto records = store_->records();
    if (!records)
        return result;
    for (const auto &record : records.value) {
        if (record.operation == DraftRecord::Publish && record.state == DraftRecord::Editing)
            result.push_back(record);
    }
    qCInfo(logDraftPersistence) << "Recoverable editing drafts:" << result.size();
    return result;
}

void DraftManager::publishPending()
{
    if (!store_) {
        qCWarning(logDraftPersistence) << "Cannot publish drafts: draft store is unavailable";
        return;
    }
    auto records = store_->records();
    if (!records) {
        qCWarning(logDraftPersistence) << "Cannot enumerate drafts for publication" << int(records.error.code)
                                       << records.error.message;
        return;
    }
    qCInfo(logDraftPersistence) << "Checking" << records.value.size()
                                << "draft records for publication; active=" << publishing_.size();
    for (const auto &record : records.value) {
        qCInfo(logDraftPersistence) << "Draft publication candidate: id=" << record.id.toString(QUuid::WithoutBraces)
                                    << "operation=" << draftOperationName(record.operation)
                                    << "state=" << draftStateName(record.state) << "storage=" << record.storageId
                                    << "remoteNotePresent=" << !record.remoteNoteId.isEmpty()
                                    << "revision=" << record.revision;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &storedRecord : records.value) {
        auto record = storedRecord;
        if (record.state == DraftRecord::Retry) {
            if (!record.retryAt.isValid())
                continue;
            if (record.retryAt > now) {
                QTimer::singleShot(qMax<qint64>(1, now.msecsTo(record.retryAt)), this, &DraftManager::publishPending);
                continue;
            }
        }
        const bool canRoute = record.operation == DraftRecord::Publish
            && (record.state == DraftRecord::NeedsRouting || record.state == DraftRecord::Ready
                || record.state == DraftRecord::Retry)
            && !publishing_.contains(record.id);
        if (canRoute && prePublicationHandler_) {
            auto       routed     = record;
            const auto routeError = prePublicationHandler_(&routed);
            if (routeError) {
                const bool retryable = routeError.code == DraftStoreError::Io
                    || routeError.code == DraftStoreError::Locked
                    || routeError.code == DraftStoreError::CryptoUnavailable;
                retry(record, routeError.message, retryable);
                continue;
            }
            if (routed.id != record.id || routed.operation != record.operation) {
                retry(record, tr("The pre-publication handler changed the draft identity"), false);
                continue;
            }
            if (!sameDraftRecord(record, routed)) {
                routed.updatedAt = now;
                if (const auto writeError = store_->write(routed)) {
                    retry(record, writeError.message, true);
                    continue;
                }
                emit draftsChanged();
                record = std::move(routed);
            }
        }
        if (record.operation == DraftRecord::Publish && record.state == DraftRecord::NeedsRouting) {
            auto target = NoteManager::instance()->defaultStorage();
            if (!target)
                continue;
            auto routed      = record;
            routed.storageId = target->systemName();
            routed.remoteNoteId.clear();
            routed.state = DraftRecord::Ready;
            routed.lastError.clear();
            routed.retryAt = {};
            if (store_->write(routed))
                continue;
            process(routed);
            continue;
        }
        if ((record.state == DraftRecord::Ready || record.state == DraftRecord::Retry
             || record.state == DraftRecord::Publishing)
            && !publishing_.contains(record.id)) {
            process(record);
        }
    }
    if (publishing_.isEmpty())
        emit publishingIdle();
}

void DraftManager::process(const DraftRecord &record)
{
    if (record.operation == DraftRecord::Delete)
        remove(record);
    else
        publish(record);
}

void DraftManager::retry(const DraftRecord &record, const QString &message, bool retryable)
{
    qCWarning(logDraftPersistence) << "Draft publication retry/failure: id=" << record.id.toString(QUuid::WithoutBraces)
                                   << "state=" << draftStateName(record.state) << "retryable=" << retryable
                                   << "message=" << message;
    constexpr qint64 MinimumDelay = 30;
    constexpr qint64 MaximumDelay = 300;
    const auto       now          = QDateTime::currentDateTimeUtc();
    qint64           delay        = MinimumDelay;
    if (record.updatedAt.isValid() && record.retryAt.isValid())
        delay = qBound(MinimumDelay, record.updatedAt.secsTo(record.retryAt) * 2, MaximumDelay);

    auto retry      = record;
    retry.state     = DraftRecord::Retry;
    retry.lastError = message;
    retry.updatedAt = now;
    retry.retryAt   = retryable ? now.addSecs(delay) : QDateTime {};
    store_->write(retry);
    emit draftPublishFailed(record.id, message);
    if (retryable)
        QTimer::singleShot(delay * 1000, this, &DraftManager::publishPending);
}

void DraftManager::resolveConflict(const DraftRecord &record, const StorageError &error, const Note &remoteNote)
{
    CONFLICT_TRACE << "Conflict trace: invoking resolver draft=" << record.id.toString(QUuid::WithoutBraces)
                   << "note=" << record.remoteNoteId << "base=" << concurrencySummary(record.backendData)
                   << "remote=" << concurrencySummary(remoteNote.backendData()) << "message=" << error.message;
    if (!conflictResolver_) {
        retry(record, error.message, false);
        return;
    }

    // An asynchronous/user-interactive resolver may outlive this turn of the
    // event loop. Persist the draft as recoverable before handing it over.
    auto recoverable      = record;
    recoverable.state     = DraftRecord::Editing;
    recoverable.lastError = error.message;
    recoverable.retryAt   = {};
    if (const auto writeError = store_->write(recoverable)) {
        emit publicationAbandoned(tr("Failed to preserve a conflicting draft: %1").arg(writeError.message));
        return;
    }

    conflictResolver_->resolve(
        { recoverable, remoteNote, error.message },
        [this, id = record.id, fallbackMessage = error.message](ConflictResolution resolution) {
            if (!store_)
                return;
            auto current = store_->load(id);
            if (!current) {
                emit publicationAbandoned(tr("Failed to load a conflicting draft: %1").arg(current.error.message));
                return;
            }

            switch (resolution.action) {
            case ConflictResolution::CreateCopy: {
                CONFLICT_TRACE << "Conflict trace: resolver action=create-copy draft="
                               << id.toString(QUuid::WithoutBraces) << "old-note=" << current.value.remoteNoteId;
                current.value.remoteNoteId.clear();
                current.value.backendData.clear();
                current.value.title = resolution.copyTitle.isEmpty() ? tr("%1 (conflict copy)").arg(current.value.title)
                                                                     : resolution.copyTitle;
                current.value.state
                    = current.value.storageId.isEmpty() ? DraftRecord::NeedsRouting : DraftRecord::Ready;
                current.value.lastError.clear();
                current.value.retryAt = {};
                if (const auto writeError = store_->write(current.value)) {
                    retry(current.value, writeError.message, false);
                    return;
                }
                if (!resolution.notification.isEmpty())
                    emit conflictResolved(resolution.notification);
                QTimer::singleShot(0, this, &DraftManager::publishPending);
                break;
            }
            case ConflictResolution::KeepDraft: {
                CONFLICT_TRACE << "Conflict trace: resolver action=keep-draft draft="
                               << id.toString(QUuid::WithoutBraces);
                current.value.state     = DraftRecord::Editing;
                current.value.lastError = fallbackMessage;
                current.value.retryAt   = {};
                if (const auto writeError = store_->write(current.value))
                    emit publicationAbandoned(tr("Failed to preserve a conflicting draft: %1").arg(writeError.message));
                break;
            }
            case ConflictResolution::Discard:
                CONFLICT_TRACE << "Conflict trace: resolver action=discard draft=" << id.toString(QUuid::WithoutBraces);
                if (const auto removeError = store_->remove(id))
                    emit publicationAbandoned(tr("Failed to discard a conflicting draft: %1").arg(removeError.message));
                break;
            }
        });
}

void DraftManager::publish(const DraftRecord &record)
{
    qCInfo(logDraftPersistence) << "Publishing draft: id=" << record.id.toString(QUuid::WithoutBraces)
                                << "storage=" << record.storageId
                                << "remoteNotePresent=" << !record.remoteNoteId.isEmpty()
                                << "revision=" << record.revision << "titleLength=" << record.title.size()
                                << "bodyLength=" << record.body.size();
    CONFLICT_TRACE << "Conflict trace: publish begin draft=" << record.id.toString(QUuid::WithoutBraces)
                   << "storage=" << record.storageId << "note=" << record.remoteNoteId
                   << "base=" << concurrencySummary(record.backendData);
    auto storage = NoteManager::instance()->storage(record.storageId);
    if (!storage || !storage->canAcceptWrites()) {
        qCWarning(logDraftPersistence) << "Target storage is unavailable for draft"
                                       << record.id.toString(QUuid::WithoutBraces) << record.storageId
                                       << "exists=" << bool(storage)
                                       << "canWrite=" << (storage ? storage->canAcceptWrites() : false);
        retry(record, tr("Target storage is unavailable"));
        return;
    }
    auto publishing  = record;
    publishing.state = DraftRecord::Publishing;
    if (store_->write(publishing))
        return;
    publishing_.insert(record.id);

    const auto save = [this, record, storage](Note note) {
        if (note.isNull()) {
            publishing_.remove(record.id);
            retry(record, tr("Target note could not be created or loaded"));
            return;
        }
        // Restore the captured concurrency token. New-note drafts may also
        // carry one-shot storage hints such as a requested modification time.
        if (!record.backendData.isEmpty())
            note.setBackendData(record.backendData);
        note.setTitle(record.title);
        note.setText(record.body, record.format);
        note.setTags(record.tags);
        note.setFolderId(record.folderId);
        note.setMedia(record.media);
        qCInfo(logDraftPersistence) << "Submitting draft to storage: draft=" << record.id.toString(QUuid::WithoutBraces)
                                    << "storage=" << storage->systemName() << "noteIdPresent=" << !note.id().isEmpty();
        auto *job = storage->saveNoteAsync(note, this);
        publishJobs_.insert(record.id, job);
        connect(job, &StorageJob::finished, this, [this, record, job]() {
            publishing_.remove(record.id);
            publishJobs_.remove(record.id);
            if (job->state() == StorageJob::Succeeded) {
                qCInfo(logDraftPersistence)
                    << "Draft publication succeeded: draft=" << record.id.toString(QUuid::WithoutBraces)
                    << "storage=" << job->result().storageId() << "noteIdPresent=" << !job->result().id().isEmpty();
                CONFLICT_TRACE << "Conflict trace: publish succeeded draft=" << record.id.toString(QUuid::WithoutBraces)
                               << "note=" << job->result().id()
                               << "result=" << concurrencySummary(job->result().backendData());
                finishPublishedDraft(record, job->result());
            } else {
                qCWarning(logDraftPersistence)
                    << "Draft publication job failed: draft=" << record.id.toString(QUuid::WithoutBraces)
                    << "state=" << int(job->state()) << "code=" << int(job->error().code)
                    << "retryable=" << job->error().retryable << "message=" << job->error().message;
                CONFLICT_TRACE << "Conflict trace: publish failed draft=" << record.id.toString(QUuid::WithoutBraces)
                               << "code=" << int(job->error().code) << "retryable=" << job->error().retryable
                               << "message=" << job->error().message;
                auto pending = store_->load(record.id);
                if (pending) {
                    if (job->error().code == StorageError::Conflict)
                        resolveConflict(pending.value, job->error());
                    else
                        retry(pending.value, job->error().message, job->error().retryable);
                }
            }
            job->deleteLater();
            if (publishing_.isEmpty())
                emit publishingIdle();
        });
    };

    if (record.remoteNoteId.isEmpty()) {
        save(storage->createNote());
        return;
    }

    auto *job = storage->loadNoteAsync(record.remoteNoteId, this);
    publishJobs_.insert(record.id, job);
    connect(job, &StorageJob::finished, this, [this, record, job, save]() mutable {
        publishJobs_.remove(record.id);
        if (job->state() == StorageJob::Succeeded) {
            auto note = job->result();
            // A return to the original contents needs no publication. This is
            // deliberately compared with the storage's current/cached note,
            // not with a full second snapshot in every DraftRecord. It is also
            // safe when the remote changed concurrently but now has identical
            // contents: keeping that remote version is the desired no-op.
            if (hasSamePublishedContents(record, note)) {
                publishing_.remove(record.id);
                finishPublishedDraft(record, note);
                job->deleteLater();
                if (publishing_.isEmpty())
                    emit publishingIdle();
                return;
            }
            CONFLICT_TRACE << "Conflict trace: remote loaded draft=" << record.id.toString(QUuid::WithoutBraces)
                           << "note=" << record.remoteNoteId << "remote=" << concurrencySummary(note.backendData())
                           << "restoring-base=" << concurrencySummary(record.backendData);
            job->deleteLater();
            save(note);
            return;
        }
        publishing_.remove(record.id);
        retry(record, job->error().message, job->error().retryable);
        job->deleteLater();
        if (publishing_.isEmpty())
            emit publishingIdle();
    });
}

void DraftManager::finishPublishedDraft(const DraftRecord &record, const Note &note)
{
    if (!store_)
        return;

    if (!record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty()
        && (record.removeSourceStorageId != note.storageId() || record.removeSourceNoteId != note.id())) {
        // Capture the destination identity before queuing source deletion. If
        // the process stops afterwards, a retry updates this exact destination
        // rather than creating a duplicate copy.
        auto completed         = record;
        completed.storageId    = note.storageId();
        completed.remoteNoteId = note.id();
        completed.backendData  = note.backendData();
        completed.state        = DraftRecord::Ready;
        completed.lastError.clear();
        completed.retryAt = {};
        if (const auto writeError = store_->write(completed)) {
            retry(record, writeError.message, true);
            return;
        }
        if (const auto removeError = queueRemoval(record.removeSourceStorageId, record.removeSourceNoteId)) {
            retry(completed, removeError.message, true);
            return;
        }
    }

    if (const auto removeError = store_->remove(record.id)) {
        retry(record, removeError.message, true);
        return;
    }
    emit draftPublished(record.id, note);
}

void DraftManager::remove(const DraftRecord &record)
{
    auto storage = NoteManager::instance()->storage(record.storageId);
    if (!storage || !storage->isAccessible()) {
        retry(record, tr("Target storage is unavailable"));
        return;
    }

    auto removing  = record;
    removing.state = DraftRecord::Publishing;
    if (store_->write(removing))
        return;

    publishing_.insert(record.id);
    auto *job = storage->removeNoteAsync(record.remoteNoteId, this);
    publishJobs_.insert(record.id, job);
    connect(job, &StorageJob::finished, this, [this, id = record.id, job]() {
        publishing_.remove(id);
        publishJobs_.remove(id);
        if (job->state() == StorageJob::Succeeded) {
            store_->remove(id);
        } else {
            auto pending = store_->load(id);
            if (pending)
                retry(pending.value, job->error().message, job->error().retryable);
        }
        job->deleteLater();
        if (publishing_.isEmpty())
            emit publishingIdle();
    });
}

void DraftManager::storageBecameReady(NoteStorage *storage)
{
    if (!store_ || !storage)
        return;

    // A draft can be moved to Retry during startup before an asynchronous
    // storage has completed init(). Once that exact storage becomes ready,
    // retry immediately instead of waiting for the generic network backoff.
    const auto records = store_->records();
    if (records) {
        for (const auto &record : records.value) {
            if (record.state != DraftRecord::Retry || !record.retryAt.isValid()
                || record.storageId != storage->systemName()) {
                continue;
            }
            auto ready      = record;
            ready.state     = DraftRecord::Ready;
            ready.lastError = {};
            ready.retryAt   = {};
            if (const auto error = store_->write(ready)) {
                qWarning() << "Failed to requeue draft after storage became ready:" << error.message;
            }
        }
    }
    QTimer::singleShot(0, this, &DraftManager::publishPending);
}

void DraftManager::storageAboutToBeRemoved(NoteStorage *storage)
{
    if (!store_ || !storage)
        return;
    auto records = store_->records();
    if (!records)
        return;
    int affected = 0;
    for (auto record : records.value) {
        if (record.storageId != storage->systemName())
            continue;
        if (auto job = publishJobs_.value(record.id))
            job->cancel();
        publishing_.remove(record.id);
        publishJobs_.remove(record.id);
        if (record.operation == DraftRecord::Delete) {
            record.state     = DraftRecord::Retry;
            record.lastError = tr("The storage plugin was disabled; deletion is still pending");
            record.retryAt   = {};
            if (!store_->write(record))
                ++affected;
            continue;
        }
        if (record.state != DraftRecord::Editing)
            record.state = DraftRecord::NeedsRouting;
        record.storageId.clear();
        record.remoteNoteId.clear();
        record.lastError = tr("The storage plugin was disabled; the remote original was left unchanged");
        record.retryAt   = {};
        if (!store_->write(record))
            ++affected;
    }
    if (affected) {
        emit publicationAbandoned(
            tr("%n note(s) could not be published because storage “%1” was disabled. The remote originals were "
               "left unchanged; local drafts will be routed as new notes.",
               nullptr, affected)
                .arg(storage->name()));
    }
}

} // namespace QtNote
