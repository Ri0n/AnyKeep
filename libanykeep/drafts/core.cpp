#include "draftmanager.h"

#include "private.h"

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

namespace AnyKeep {

Q_LOGGING_CATEGORY(logDraftPersistence, "anykeep.persistence.drafts")

namespace DraftManagerPrivate {

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

} // namespace DraftManagerPrivate

using DraftManagerPrivate::draftOperationName;
using DraftManagerPrivate::draftStateName;

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
    const QString draftsPath = Utils::anykeepDataDir() + QStringLiteral("/drafts");
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

    const QString   draftsPath = Utils::anykeepDataDir() + QStringLiteral("/drafts");
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

} // namespace AnyKeep
