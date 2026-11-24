#include "backuprestorejob.h"

#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "localmediastore.h"
#include "notemanager.h"

#include <QMetaObject>
#include <QSet>
#include <QThread>

#include <atomic>
#include <utility>

namespace AnyKeep {

struct BackupRestoreJob::WorkerState {
    std::atomic_bool cancelled { false };
    QString error;
    std::unique_ptr<PortableBackupRestore> prepared;
    DraftStoreResult<QList<RecoveredNoteCopy>> copies;
};

BackupRestoreJob::BackupRestoreJob(NoteManager &notes, DraftManager &drafts, FolderCatalogManager &folders,
                                   LocalMediaStore &mediaStore, QObject *owner) :
    StorageJob(owner), notes_(&notes), drafts_(&drafts), folders_(&folders), mediaStore_(mediaStore)
{
}

BackupRestoreJob::~BackupRestoreJob()
{
    stopComparison();
    stopWorker();
}

BackupRestoreJob *BackupRestoreJob::restoreDirectory(const QString &directory, const QByteArray &previewManifestSha256,
                                                    const QString &privateParent, NoteManager &notes,
                                                    DraftManager &drafts, FolderCatalogManager &folders,
                                                    LocalMediaStore &mediaStore, QObject *owner)
{
    auto *job = new BackupRestoreJob(notes, drafts, folders, mediaStore, owner);
    job->start();
    job->startPreparation(directory, previewManifestSha256, privateParent);
    return job;
}

void BackupRestoreJob::setPhase(Phase phase)
{
    if (phase_ == phase)
        return;
    phase_ = phase;
    emit phaseChanged(phase);
}

void BackupRestoreJob::stopWorker()
{
    if (!worker_)
        return;
    workerState_->cancelled.store(true);
    disconnect(worker_, nullptr, this, nullptr);
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
    workerState_.reset();
}

void BackupRestoreJob::stopComparison()
{
    if (auto *job = comparison_.data()) {
        comparison_.clear();
        disconnect(job, nullptr, this, nullptr);
        if (!job->isFinished())
            job->cancel();
        job->deleteLater();
    }
}

void BackupRestoreJob::startPreparation(const QString &directory, const QByteArray &digest,
                                         const QString &privateParent)
{
    workerState_ = std::make_shared<WorkerState>();
    auto state = workerState_;
    worker_ = QThread::create([this, state, directory, digest, privateParent] {
        const auto progress = [this, state](int done, int total) {
            if (state->cancelled.load())
                return false;
            QMetaObject::invokeMethod(this, [this, done, total] {
                if (!isFinished() && phase_ == Preparing)
                    emit progressChanged(done, total);
            }, Qt::QueuedConnection);
            return true;
        };
        state->prepared = PortableBackupRestore::prepareDirectory(directory, digest, privateParent, &state->error,
                                                                  progress, [state] { return !state->cancelled.load(); });
    });
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, &BackupRestoreJob::preparationFinished, Qt::QueuedConnection);
    worker_->start();
}

void BackupRestoreJob::preparationFinished()
{
    if (!worker_)
        return;
    worker_->wait();
    auto state = workerState_;
    const bool cancelled = state->cancelled.load();
    stopWorker();
    if (isFinished())
        return;
    if (cancelled) {
        StorageJob::cancel();
        return;
    }
    if (!state->prepared) {
        fail({ StorageError::Io, state->error, false });
        return;
    }
    prepared_ = std::move(state->prepared);
    if (!notes_ || !drafts_ || !folders_ || !folders_->isAvailable()) {
        fail({ StorageError::Unavailable, tr("The recovery profile is unavailable"), false });
        return;
    }
    setPhase(Comparing);
    if (isFinished())
        return;
    if (!notes_ || !drafts_ || !folders_) {
        fail({ StorageError::Unavailable, tr("The recovery profile is unavailable"), false });
        return;
    }
    auto *job = BackupRestorePreviewJob::compare(prepared_->storages(), notes_->storages(true), *drafts_,
                                                 folders_->snapshot(), this);
    comparison_ = job;
    connect(job, &BackupRestorePreviewJob::progressChanged, this, &BackupRestoreJob::progressChanged);
    connect(job, &StorageJob::finished, this, &BackupRestoreJob::comparisonFinished, Qt::QueuedConnection);
    if (job->isFinished())
        QMetaObject::invokeMethod(this, &BackupRestoreJob::comparisonFinished, Qt::QueuedConnection);
}

void BackupRestoreJob::comparisonFinished()
{
    if (isFinished() || !comparison_)
        return;
    const auto state = comparison_->state();
    const auto error = comparison_->error();
    if (state == Succeeded)
        decisions_ = comparison_->result();
    stopComparison();
    if (state != Succeeded) {
        if (state == Cancelled)
            StorageJob::cancel();
        else
            fail(error);
        return;
    }
    setPhase(AwaitingSelection);
}

StorageError BackupRestoreJob::validateChoices(const QList<Choice> &choices, bool importFolders) const
{
    if (!prepared_ || !notes_ || !drafts_ || !folders_ || !drafts_->isReady() || !folders_->isAvailable())
        return { StorageError::Unavailable, tr("The recovery profile is unavailable"), false };
    if (choices.size() != decisions_.size())
        return { StorageError::Other, tr("Every backup note needs an explicit recovery choice"), false };

    FolderCatalog candidate;
    if (const auto error = candidate.replaceSnapshot(folders_->snapshot()))
        return { StorageError::Conflict, error.message, false };
    if (importFolders) {
        if (const auto error = candidate.mergeRecoveredFolders(prepared_->folders()))
            return { StorageError::Conflict, error.message, false };
    }

    QSet<qsizetype> seen;
    for (const auto &choice : choices) {
        if (choice.decisionIndex < 0 || choice.decisionIndex >= decisions_.size()
            || seen.contains(choice.decisionIndex))
            return { StorageError::Other, tr("Invalid or duplicate recovery choice"), false };
        seen.insert(choice.decisionIndex);
        if (choice.action == Choice::Skip)
            continue;
        if (choice.action != Choice::KeepDraft && choice.action != Choice::CreateCopy)
            return { StorageError::Other, tr("Unknown recovery action"), false };
        const auto &decision = decisions_.at(choice.decisionIndex);
        const auto &note = prepared_->storages().at(decision.storageIndex).notes.at(decision.noteIndex).snapshot;
        if (!note.folderId().isNull() && !FolderCatalog::isRecycleBinId(note.folderId())) {
            const auto *folder = candidate.folder(note.folderId());
            if (!folder || folder->tombstone)
                return { StorageError::Conflict, tr("A selected note refers to an unavailable folder"), false };
        }
        if (!choice.destinationStorageId.isEmpty()) {
            const auto storage = notes_->storage(choice.destinationStorageId);
            if (!storage || storage->availableFormats().isEmpty())
                return { StorageError::Unavailable, tr("The selected destination is no longer configured"), false };
            if (!note.media().isEmpty() && !storage->supportsMedia())
                return { StorageError::Other, tr("The selected destination does not support attachments"), false };
        }
    }
    return {};
}

StorageError BackupRestoreJob::apply(const QList<Choice> &choices, bool importFolders, bool acceptIncomplete)
{
    if (isFinished() || phase_ != AwaitingSelection)
        return { StorageError::Other, tr("Recovery is not awaiting a selection"), false };
    if (!prepared_->inspection().complete && !acceptIncomplete)
        return { StorageError::Other, tr("Confirm the incomplete backup before restoring it"), false };
    if (const auto error = validateChoices(choices, importFolders))
        return error;

    choices_ = choices;
    importFolders_ = importFolders;
    result_.skipped = 0;
    for (const auto &choice : choices_) {
        if (choice.action == Choice::Skip)
            ++result_.skipped;
    }
    if (result_.skipped == choices_.size()) {
        commit({});
        return {};
    }

    setPhase(PreparingMedia);
    if (isFinished())
        return {};
    mediaStore_.initializeAsync(this, [this](QString error) {
        if (isFinished())
            return;
        if (!error.isEmpty()) {
            fail({ StorageError::Authentication, error, false });
            return;
        }
        startMediaCopy();
    });
    return {};
}

void BackupRestoreJob::startMediaCopy()
{
    setPhase(CopyingMedia);
    if (isFinished())
        return;
    QList<PortableBackupRestore::CopySelection> selection;
    QStringList titles;
    for (const auto &choice : choices_) {
        if (choice.action == Choice::Skip)
            continue;
        const auto &decision = decisions_.at(choice.decisionIndex);
        selection.append({ decision.storageIndex, decision.noteIndex, choice.destinationStorageId });
        QString title = choice.title;
        if (title.isEmpty() && choice.action == Choice::CreateCopy
            && decision.status == BackupRestoreDecision::Conflict
            && decision.resolution.action == ConflictResolution::CreateCopy)
            title = decision.resolution.copyTitle;
        titles.append(title);
    }
    workerState_ = std::make_shared<WorkerState>();
    auto state = workerState_;
    auto *prepared = prepared_.get();
    auto *media = &mediaStore_;
    result_.mediaMayRemain = true;
    worker_ = QThread::create([this, state, prepared, media, selection, titles] {
        const auto progress = [this, state](int done, int total) {
            if (state->cancelled.load())
                return false;
            QMetaObject::invokeMethod(this, [this, done, total] {
                if (!isFinished() && phase_ == CopyingMedia)
                    emit progressChanged(done, total);
            }, Qt::QueuedConnection);
            return true;
        };
        state->copies = prepared->prepareCopies(selection, *media, progress, [state] { return !state->cancelled.load(); });
        if (state->copies) {
            for (qsizetype i = 0; i < titles.size(); ++i) {
                if (!titles.at(i).isEmpty())
                    state->copies.value[i].snapshot.setTitle(titles.at(i));
            }
        }
    });
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, &BackupRestoreJob::mediaCopyFinished, Qt::QueuedConnection);
    worker_->start();
}

void BackupRestoreJob::mediaCopyFinished()
{
    if (!worker_)
        return;
    worker_->wait();
    auto state = workerState_;
    const bool cancelled = state->cancelled.load();
    stopWorker();
    if (isFinished())
        return;
    if (cancelled) {
        StorageJob::cancel();
        return;
    }
    if (!state->copies) {
        fail({ StorageError::Io, state->copies.error.message, false });
        return;
    }
    commit(state->copies.value);
}

void BackupRestoreJob::commit(const QList<RecoveredNoteCopy> &copies)
{
    // Check again after the worker: destination or folder configuration may
    // have changed while importing media. All choices create new drafts, so
    // a changed remote body cannot authorize an overwrite.
    if (const auto error = validateChoices(choices_, importFolders_)) {
        fail(error);
        return;
    }
    setPhase(Committing);
    // Beyond this boundary cancellation is not accepted. Do not report a
    // cancelled result for a batch that has already committed durable data.
    if (importFolders_) {
        if (!folders_) {
            fail({ StorageError::Unavailable, tr("The folder catalog is unavailable"), false });
            return;
        }
        if (const auto error = folders_->mergeRecoveredFolders(prepared_->folders())) {
            fail({ StorageError::Conflict, error.message, false });
            return;
        }
        result_.foldersMerged = true;
    }
    if (!copies.isEmpty()) {
        if (!drafts_) {
            fail({ StorageError::Unavailable, tr("The draft manager is unavailable"), false });
            return;
        }
        const auto staged = drafts_->stageRecoveredCopies(copies);
        result_.draftIds = staged.value;
        if (!staged) {
            fail({ StorageError::Io, staged.error.message, false });
            return;
        }
    }
    result_.mediaMayRemain = false;
    complete();
}

void BackupRestoreJob::cancel()
{
    if (isFinished() || phase_ == Committing)
        return;
    // A running worker must finish before the private prepared tree can be
    // destroyed. Cancellation is cooperative; do not block the UI here.
    if (worker_) {
        workerState_->cancelled.store(true);
        return;
    }
    stopComparison();
    StorageJob::cancel();
}

} // namespace AnyKeep
