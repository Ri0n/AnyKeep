#include "noteruleapplicationcontroller.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "folderoperationscontroller.h"
#include "notemanager.h"
#include "noterulemanager.h"
#include "notesindex.h"
#include "notestorage.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QTimer>
#include <algorithm>

#include <utility>

namespace QtNote {

Q_LOGGING_CATEGORY(logRuleApplication, "qtnote.rules")

NoteRuleApplicationController::NoteRuleApplicationController(NoteRuleManager      *ruleManager,
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
    static auto *controller
        = new NoteRuleApplicationController(nullptr, nullptr, nullptr, nullptr, QCoreApplication::instance());
    return controller;
}

void NoteRuleApplicationController::initialize()
{
    if (initialized_)
        return;
    initialized_ = true;

    if (draftManager_) {
        draftManager_->setPrePublicationHandler([this](DraftRecord *record) { return routeDraft(record); });
        connect(draftManager_, &DraftManager::draftPublished, this,
                [this](const QUuid &draftId, const Note &note) { handleDraftPublished(draftId, note); });
    }

    if (noteManager_ && noteManager_->notesIndex()) {
        connect(noteManager_->notesIndex(), &NotesIndex::storageNotesChanged, this,
                [this](const QString &storageId) { queueFolderOverlayImport(storageId); });
        connect(noteManager_, &NoteManager::storageRemoved, this, [this](const NoteStorage::Ptr &storage) {
            if (storage)
                pendingFolderOverlayStorageIds_.remove(storage->systemName());
        });
    }
    if (ruleManager_) {
        connect(ruleManager_, &NoteRuleManager::rulesChanged, this,
                &NoteRuleApplicationController::queueAllFolderOverlayImports);
        connect(ruleManager_, &NoteRuleManager::availabilityChanged, this, [this](bool available) {
            if (available)
                queueAllFolderOverlayImports();
        });
    }
    if (folderCatalogManager_) {
        connect(folderCatalogManager_, &FolderCatalogManager::availabilityChanged, this, [this](bool available) {
            if (available)
                queueAllFolderOverlayImports();
        });
    }
    queueAllFolderOverlayImports();
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

void NoteRuleApplicationController::enqueueMarkers(const QList<QUuid> &ruleIds, const NoteRuleEvaluationInput &input)
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

void NoteRuleApplicationController::queueFolderOverlayImport(const QString &storageId)
{
    if (!supportsFolderOverlayImport(storageId))
        return;
    pendingFolderOverlayStorageIds_.insert(storageId);
    if (folderOverlayImportScheduled_)
        return;
    folderOverlayImportScheduled_ = true;
    QTimer::singleShot(0, this, &NoteRuleApplicationController::processFolderOverlayImports);
}

void NoteRuleApplicationController::queueAllFolderOverlayImports()
{
    if (!noteManager_)
        return;
    for (const auto &storage : noteManager_->storages()) {
        if (storage)
            queueFolderOverlayImport(storage->systemName());
    }
}

void NoteRuleApplicationController::processFolderOverlayImports()
{
    folderOverlayImportScheduled_ = false;
    const auto storageIds         = std::exchange(pendingFolderOverlayStorageIds_, {});
    for (const auto &storageId : storageIds)
        importFolderOverlays(storageId);
}

void NoteRuleApplicationController::importFolderOverlays(const QString &storageId)
{
    if (!ruleManager_ || !ruleManager_->isAvailable() || !folderCatalogManager_ || !folderCatalogManager_->isAvailable()
        || !noteManager_ || !noteManager_->notesIndex() || !supportsFolderOverlayImport(storageId)) {
        return;
    }

    for (const auto &note : noteManager_->notesIndex()->notes(storageId))
        applyFolderOverlayRules(note);
}

void NoteRuleApplicationController::loadAndImportFolderOverlay(const QString &storageId, const QString &noteId)
{
    if (!noteManager_ || !supportsFolderOverlayImport(storageId) || noteId.isEmpty())
        return;
    const auto key = overlayLoadKey(storageId, noteId);
    if (pendingFolderOverlayLoads_.contains(key))
        return;
    pendingFolderOverlayLoads_.insert(key);

    auto      *job      = noteManager_->loadNoteAsync(storageId, noteId, this);
    const auto finished = [this, job, key, storageId, noteId]() {
        pendingFolderOverlayLoads_.remove(key);
        if (job->state() == StorageJob::Succeeded) {
            if (supportsFolderOverlayImport(storageId))
                applyFolderOverlayRules(job->result());
        } else if (job->state() != StorageJob::Cancelled) {
            reportFailure(storageId, noteId, job->error().message);
        }
        job->deleteLater();
    };
    connect(job, &StorageJob::finished, this, finished);
    if (job->isFinished())
        QTimer::singleShot(0, this, finished);
}

void NoteRuleApplicationController::applyFolderOverlayRules(const Note &note)
{
    const auto input = overlayInput(note);
    if (input.storageId.isEmpty() || input.noteId.isEmpty() || !folderCatalogManager_
        || !folderCatalogManager_->isAvailable()) {
        return;
    }

    // An existing overlay is either an earlier import or a direct user choice.
    // In both cases this restricted Tomboy pass must not overwrite it.
    if (!folderCatalogManager_->catalog().folderForNote(input.storageId, input.noteId).isNull())
        return;

    const auto evaluation = evaluateFolderOverlayRules(input);
    if (evaluation.error) {
        reportFailure(input.storageId, input.noteId, evaluation.error.message);
        return;
    }
    if (evaluation.requiresText) {
        if (!input.textAvailable)
            loadAndImportFolderOverlay(input.storageId, input.noteId);
        else
            reportFailure(input.storageId, input.noteId, tr("A folder rule could not evaluate note text"));
        return;
    }
    applyFolderOverlayEvaluation(input, evaluation);
}

void NoteRuleApplicationController::applyFolderOverlayEvaluation(const NoteRuleEvaluationInput &input,
                                                                 const NoteRuleEvaluation      &evaluation)
{
    if (!evaluation.folderId || evaluation.folderId->isNull())
        return;
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable())
        return;
    if (!folderCatalogManager_->catalog().folder(*evaluation.folderId)) {
        reportFailure(input.storageId, input.noteId, tr("A rule refers to a folder that no longer exists"));
        return;
    }
    if (!folderCatalogManager_->catalog().folderForNote(input.storageId, input.noteId).isNull())
        return;
    if (!folderOperations_->storeOverlayAssignment(input.storageId, input.noteId, *evaluation.folderId)) {
        reportFailure(input.storageId, input.noteId, folderOperations_->errorString());
        return;
    }
    enqueueMarkers(evaluation.matchedRuleIds, input);
}

bool NoteRuleApplicationController::supportsFolderOverlayImport(const QString &storageId) const
{
    if (!noteManager_ || storageId.isEmpty())
        return false;
    const auto storage = noteManager_->storage(storageId);
    return storage && storage->supportsFolderRuleOverlayImport();
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

NoteRuleEvaluation NoteRuleApplicationController::evaluateFolderOverlayRules(const NoteRuleEvaluationInput &input) const
{
    if (!ruleManager_ || !ruleManager_->isAvailable())
        return {};

    QList<NoteRule> folderRules;
    for (auto rule : ruleManager_->rules()) {
        rule.actions.erase(std::remove_if(rule.actions.begin(), rule.actions.end(),
                                          [](const NoteRuleAction &action) {
                                              return action.kind != NoteRuleActionKind::AssignFolder;
                                          }),
                           rule.actions.end());
        if (!rule.actions.isEmpty())
            folderRules.append(std::move(rule));
    }
    return NoteRuleEvaluator::evaluate(folderRules, input);
}

NoteRuleEvaluationInput NoteRuleApplicationController::evaluationInput(const DraftRecord &record) const
{
    NoteRuleEvaluationInput input;
    const bool transferPending = !record.removeSourceStorageId.isEmpty() && !record.removeSourceNoteId.isEmpty();
    input.storageId            = transferPending ? record.removeSourceStorageId : record.storageId;
    input.noteId               = transferPending ? record.removeSourceNoteId : record.remoteNoteId;
    input.title                = record.title;
    input.tags                 = record.tags;
    input.text                 = record.body;
    input.textAvailable        = true;
    return input;
}

NoteRuleEvaluationInput NoteRuleApplicationController::overlayInput(const Note &note)
{
    NoteRuleEvaluationInput input;
    input.storageId     = note.storageId();
    input.noteId        = note.id();
    input.title         = note.title();
    input.tags          = note.tags();
    input.textAvailable = note.isLoaded();
    if (input.textAvailable)
        input.text = note.text();
    return input;
}

QString NoteRuleApplicationController::markerBatchKey(const NoteRuleEvaluationInput &input)
{
    return input.storageId + QChar(0x1f) + input.noteId + QChar(0x1f)
        + QString::fromLatin1(NoteRuleEvaluator::inputFingerprint(input).toHex());
}

QString NoteRuleApplicationController::overlayLoadKey(const QString &storageId, const QString &noteId)
{
    return storageId + QChar(0x1f) + noteId;
}

} // namespace QtNote
