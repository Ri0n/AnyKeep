#include "noteruleapplicationcontroller.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "folderoperationscontroller.h"
#include "notemanager.h"
#include "noterulemanager.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QTimer>

#include <utility>

namespace QtNote {

Q_LOGGING_CATEGORY(logRuleApplication, "qtnote.rules")

NoteRuleApplicationController::NoteRuleApplicationController(NoteRuleManager *ruleManager,
                                                             FolderCatalogManager *folderCatalogManager,
                                                             NoteManager *noteManager, DraftManager *draftManager,
                                                             QObject *parent) :
    QObject(parent), ruleManager_(ruleManager ? ruleManager : NoteRuleManager::instance()),
    folderCatalogManager_(folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance()),
    noteManager_(noteManager ? noteManager : NoteManager::instance()),
    draftManager_(draftManager ? draftManager : DraftManager::instance())
{
    folderOperations_ = new FolderOperationsController(folderCatalogManager_, noteManager_, this);
}

NoteRuleApplicationController::~NoteRuleApplicationController()
{
    if (draftManager_)
        draftManager_->setPrePublicationHandler({});
}

NoteRuleApplicationController *NoteRuleApplicationController::instance()
{
    static auto *controller = new NoteRuleApplicationController(nullptr, nullptr, nullptr, nullptr,
                                                                 QCoreApplication::instance());
    return controller;
}

void NoteRuleApplicationController::initialize()
{
    if (initialized_)
        return;
    initialized_ = true;

    if (!draftManager_)
        return;
    draftManager_->setPrePublicationHandler(
        [this](DraftRecord *record) { return routeDraft(record); });
    connect(draftManager_, &DraftManager::draftPublished, this,
            [this](const QUuid &draftId, const Note &note) { handleDraftPublished(draftId, note); });
}

DraftStoreError NoteRuleApplicationController::routeDraft(DraftRecord *record)
{
    if (!record || record->operation != DraftRecord::Publish)
        return {};
    publicationMarkers_.remove(record->id);
    if (!ruleManager_ || !ruleManager_->isAvailable())
        return {};

    // Destination persistence already succeeded; only the source-removal leg
    // remains. Never create a second destination from this recovered record.
    if (!record->removeSourceStorageId.isEmpty() && !record->remoteNoteId.isEmpty())
        return {};

    const auto input      = evaluationInput(*record);
    const auto evaluation = evaluateRules(input);
    if (evaluation.error) {
        reportFailure(input.storageId, input.noteId, evaluation.error.message);
        return { DraftStoreError::InvalidArgument, evaluation.error.message };
    }
    if (evaluation.requiresText) {
        // A draft always contains its full text. Treat this as a defensive
        // failure if a future evaluator adds a condition that cannot use it.
        const auto message = tr("A publication rule requires unavailable note text");
        reportFailure(input.storageId, input.noteId, message);
        return { DraftStoreError::InvalidArgument, message };
    }
    if (evaluation.matchedRuleIds.isEmpty())
        return {};

    if (evaluation.folderId && !record->folderUserOverride) {
        const auto folderId = *evaluation.folderId;
        if (!folderId.isNull()) {
            if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable()) {
                const auto message = folderCatalogManager_ ? folderCatalogManager_->lastError()
                                                            : tr("The encrypted folder catalog is unavailable");
                reportFailure(input.storageId, input.noteId, message);
                return { DraftStoreError::Locked, message };
            }
            if (!folderCatalogManager_->catalog().folder(folderId)) {
                const auto message = tr("A rule refers to a folder that no longer exists");
                reportFailure(input.storageId, input.noteId, message);
                return { DraftStoreError::InvalidArgument, message };
            }
        }
        record->folderId = folderId;
    }

    if (!evaluation.storageId.isEmpty() && evaluation.storageId != record->storageId) {
        if (!draftManager_) {
            const auto message = tr("Draft storage is unavailable");
            reportFailure(input.storageId, input.noteId, message);
            return { DraftStoreError::Locked, message };
        }
        if (const auto routeError = draftManager_->retargetDraftForPublication(record, evaluation.storageId)) {
            reportFailure(input.storageId, input.noteId, routeError.message);
            return routeError;
        }
    }

    // Encryption is deliberately not enforced yet. It does not prevent the
    // supported folder and storage actions from being applied in this same
    // publication transaction.
    if (!input.storageId.isEmpty() && !input.noteId.isEmpty())
        publicationMarkers_.insert(record->id, { input, evaluation.matchedRuleIds });
    return {};
}

void NoteRuleApplicationController::handleDraftPublished(const QUuid &draftId, const Note &note)
{
    if (!note.isNull() && !note.storageId().isEmpty() && !note.id().isEmpty() && folderCatalogManager_
        && folderCatalogManager_->isAvailable()
        && !folderOperations_->storeOverlayAssignment(note.storageId(), note.id(), note.folderId())) {
        reportFailure(note.storageId(), note.id(), folderOperations_->errorString());
    }

    const auto pending = publicationMarkers_.take(draftId);
    if (!pending.ruleIds.isEmpty())
        enqueueMarkers(pending.ruleIds, pending.input);
}

void NoteRuleApplicationController::flushMarkers()
{
    markerFlushScheduled_ = false;
    if (!ruleManager_ || !ruleManager_->isAvailable()) {
        pendingMarkers_.clear();
        return;
    }
    QList<NoteRuleApplication> applications;
    applications.reserve(pendingMarkers_.size());
    for (const auto &pending : std::as_const(pendingMarkers_))
        applications.append({ pending.ruleIds, pending.input });
    pendingMarkers_.clear();
    if (const auto error = ruleManager_->recordApplied(applications))
        reportFailure({}, {}, error.message);
}

void NoteRuleApplicationController::enqueueMarkers(const QList<QUuid> &ruleIds,
                                                    const NoteRuleEvaluationInput &input)
{
    if (ruleIds.isEmpty() || input.storageId.isEmpty() || input.noteId.isEmpty())
        return;
    auto &pending = pendingMarkers_[markerBatchKey(input)];
    pending.input = input;
    for (const auto &id : ruleIds) {
        if (!id.isNull() && !pending.ruleIds.contains(id))
            pending.ruleIds.append(id);
    }
    if (markerFlushScheduled_)
        return;
    markerFlushScheduled_ = true;
    QTimer::singleShot(0, this, &NoteRuleApplicationController::flushMarkers);
}

void NoteRuleApplicationController::reportFailure(const QString &storageId, const QString &noteId,
                                                   const QString &message)
{
    if (message.isEmpty())
        return;
    qCWarning(logRuleApplication) << "Rule application failed: storage=" << storageId
                                  << "noteIdPresent=" << !noteId.isEmpty() << message;
    emit ruleApplicationFailed(storageId, noteId, message);
}

NoteRuleEvaluation NoteRuleApplicationController::evaluateRules(const NoteRuleEvaluationInput &input) const
{
    // Applied markers are audit/outcome history. Filtering rules by them here
    // would skip a matching stop-processing rule and change the rule order.
    return ruleManager_ ? ruleManager_->evaluate(input) : NoteRuleEvaluation {};
}

NoteRuleEvaluationInput NoteRuleApplicationController::evaluationInput(const DraftRecord &record) const
{
    NoteRuleEvaluationInput input;
    const bool transferPending
        = !record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty();
    input.storageId    = transferPending ? record.removeSourceStorageId : record.storageId;
    input.noteId       = transferPending ? record.removeSourceNoteId : record.remoteNoteId;
    input.title        = record.title;
    input.tags         = record.tags;
    input.text         = record.body;
    input.textAvailable = true;
    return input;
}

QString NoteRuleApplicationController::markerBatchKey(const NoteRuleEvaluationInput &input)
{
    return input.storageId + QChar(0x1f) + input.noteId + QChar(0x1f)
        + QString::fromLatin1(NoteRuleEvaluator::inputFingerprint(input).toHex());
}

} // namespace QtNote
