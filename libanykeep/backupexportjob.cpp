#include "backupexportjob.h"

#include "localmediastore.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <utility>

namespace AnyKeep {

struct BackupExportJob::WorkerState {
    enum class Gate { Open, Cancelled, Publishing };
    std::atomic<Gate> gate { Gate::Open };
    PortableBackupResult result;

    void requestCancellation()
    {
        auto expected = Gate::Open;
        gate.compare_exchange_strong(expected, Gate::Cancelled);
    }

    bool mayContinue() const { return gate.load() != Gate::Cancelled; }

    bool beginPublish()
    {
        auto expected = Gate::Open;
        return gate.compare_exchange_strong(expected, Gate::Publishing);
    }
};

BackupExportJob::BackupExportJob(const QString &directory, LocalMediaStore &mediaStore, QObject *owner) :
    StorageJob(owner), directory_(directory), mediaStore_(mediaStore)
{
}

BackupExportJob::~BackupExportJob()
{
    if (snapshotJob_ && !snapshotJob_->isFinished()) {
        disconnect(snapshotJob_, nullptr, this, nullptr);
        snapshotJob_->cancel();
    }
    if (worker_) {
        // The worker owns a copy of the detached snapshot. It does not wait
        // for this event loop, so shutdown can safely join it before releasing
        // the media store or unloading application components.
        disconnect(worker_, nullptr, this, nullptr);
        workerState_->requestCancellation();
        worker_->wait();
        delete worker_;
    }
}

BackupExportJob *BackupExportJob::exportDirectory(const QString &directory, NoteManager &notes, DraftManager &drafts,
                                                    FolderCatalogManager &folders, LocalMediaStore &mediaStore,
                                                    QObject *owner)
{
    auto *job = new BackupExportJob(directory, mediaStore, owner);
    job->start();
    job->observeSnapshot(BackupSnapshotJob::collect(notes, drafts, folders, job));
    return job;
}

BackupExportJob *BackupExportJob::exportDirectory(const QString &directory,
                                                    const QMap<QString, NoteStorage::Ptr> &providers,
                                                    DraftManager &drafts, const FolderCatalogSnapshot &folders,
                                                    LocalMediaStore &mediaStore, QObject *owner)
{
    auto *job = new BackupExportJob(directory, mediaStore, owner);
    job->start();
    job->observeSnapshot(BackupSnapshotJob::collect(providers, drafts, folders, job));
    return job;
}

void BackupExportJob::observeSnapshot(BackupSnapshotJob *job)
{
    snapshotJob_ = job;
    connect(job, &BackupSnapshotJob::progressChanged, this, &BackupExportJob::progressChanged);
    connect(job, &StorageJob::finished, this, &BackupExportJob::snapshotFinished, Qt::QueuedConnection);
    if (job->isFinished())
        QMetaObject::invokeMethod(this, &BackupExportJob::snapshotFinished, Qt::QueuedConnection);
}

void BackupExportJob::snapshotFinished()
{
    if (isFinished() || !snapshotJob_)
        return;

    auto *job = snapshotJob_.data();
    snapshotJob_.clear();
    const auto state = job->state();
    const auto error = job->error();
    if (state == Succeeded)
        snapshot_ = job->result();
    job->deleteLater();

    if (state == Cancelled) {
        StorageJob::cancel();
        return;
    }
    if (state != Succeeded) {
        result_.error = error.message;
        fail(error);
        return;
    }

    setPhase(PreparingMedia);
    if (isFinished())
        return;

    // Never initiate a keychain request from the file writer's thread.
    mediaStore_.initializeAsync(this, [this](QString errorText) {
        if (isFinished())
            return;
        if (!errorText.isEmpty()) {
            result_.error = errorText;
            fail({ StorageError::Authentication, errorText, false });
            return;
        }
        startWriter();
    });
}

void BackupExportJob::setPhase(Phase phase)
{
    if (phase_ == phase)
        return;
    phase_ = phase;
    emit phaseChanged(phase);
}

void BackupExportJob::startWriter()
{
    setPhase(Writing);
    if (isFinished())
        return;

    workerState_ = std::make_shared<WorkerState>();
    auto state = workerState_;
    auto *media = &mediaStore_;
    worker_ = QThread::create([this, state, media, directory = directory_, snapshot = std::move(snapshot_)] {
        const auto progress = [this, state](int done, int total) {
            if (!state->mayContinue())
                return false;
            QMetaObject::invokeMethod(this, [this, done, total] {
                if (!isFinished())
                    emit progressChanged(done, total);
            }, Qt::QueuedConnection);
            return true;
        };

        state->result = PortableBackup::exportDirectory(
            directory, snapshot.storages, snapshot.folders, *media, progress,
            [state] { return state->mayContinue(); }, [state] { return state->beginPublish(); });
    });
    worker_->setParent(this);
    connect(worker_, &QThread::finished, this, &BackupExportJob::writerFinished, Qt::QueuedConnection);
    worker_->start();
}

void BackupExportJob::writerFinished()
{
    if (!worker_)
        return;
    worker_->wait();
    delete worker_;
    worker_ = nullptr;

    result_ = std::move(workerState_->result);
    const bool cancelled = workerState_->gate.load() == WorkerState::Gate::Cancelled;
    workerState_.reset();
    if (cancelled) {
        result_.complete = false;
        StorageJob::cancel();
    } else if (!result_) {
        fail({ StorageError::Io, result_.error, false });
    } else {
        complete();
    }
}

void BackupExportJob::cancel()
{
    if (isFinished())
        return;
    if (worker_) {
        // Cancellation and publishing compete for the same gate. Once the
        // rename has been authorized, report the actual filesystem outcome;
        // a successfully published directory must not be reported cancelled.
        workerState_->requestCancellation();
        return;
    }
    if (snapshotJob_) {
        auto *job = snapshotJob_.data();
        snapshotJob_.clear();
        disconnect(job, nullptr, this, nullptr);
        job->cancel();
        job->deleteLater();
    }
    snapshot_ = {};
    StorageJob::cancel();
}

} // namespace AnyKeep
