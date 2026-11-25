#include "backupexportjob.h"

#include "localmediastore.h"
#include "protectedbackup.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QTemporaryDir>

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

bool BackupExportJob::configureProtection(const QString &privateParent, const QString &password)
{
    const QFileInfo parent(privateParent);
    if (password.isEmpty() || !QDir::isAbsolutePath(privateParent) || !parent.isDir()
        || parent.isSymLink() || parent.canonicalFilePath().isEmpty()) {
        result_.error = tr("A password and an existing private staging directory are required");
        fail({ StorageError::Other, result_.error, false });
        return false;
    }
    privateParent_ = parent.canonicalFilePath();
    password_ = password;
    return true;
}

BackupExportJob *BackupExportJob::exportProtectedDirectory(
    const QString &directory, const QString &privateParent, const QString &password,
    NoteManager &notes, DraftManager &drafts, FolderCatalogManager &folders, LocalMediaStore &mediaStore,
    QObject *owner)
{
    auto *job = new BackupExportJob(directory, mediaStore, owner);
    job->start();
    if (job->configureProtection(privateParent, password))
        job->observeSnapshot(BackupSnapshotJob::collect(notes, drafts, folders, job));
    return job;
}

BackupExportJob *BackupExportJob::exportProtectedDirectory(
    const QString &directory, const QString &privateParent, const QString &password,
    const QMap<QString, NoteStorage::Ptr> &providers, DraftManager &drafts,
    const FolderCatalogSnapshot &folders, LocalMediaStore &mediaStore, QObject *owner)
{
    auto *job = new BackupExportJob(directory, mediaStore, owner);
    job->start();
    if (job->configureProtection(privateParent, password))
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
        password_.clear();
        StorageJob::cancel();
        return;
    }
    if (state != Succeeded) {
        password_.clear();
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
            password_.clear();
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
    worker_ = QThread::create([this, state, media, directory = directory_, privateParent = privateParent_,
                               password = std::exchange(password_, {}), snapshot = std::move(snapshot_)] {
        const auto progress = [this, state](int done, int total) {
            if (!state->mayContinue())
                return false;
            QMetaObject::invokeMethod(this, [this, done, total] {
                if (!isFinished())
                    emit progressChanged(done, total);
            }, Qt::QueuedConnection);
            return true;
        };

        const auto keepGoing = [state] { return state->mayContinue(); };
        const auto beforePublish = [state] { return state->beginPublish(); };
        if (password.isEmpty()) {
            state->result = PortableBackup::exportDirectory(directory, snapshot.storages, snapshot.folders, *media,
                                                              progress, keepGoing, beforePublish);
            return;
        }

        // The ordinary writer is reused unchanged, but its intermediate
        // rename must not close the final publication gate.
        QTemporaryDir privateStage(QDir(privateParent).filePath(QStringLiteral("anykeep-protect-XXXXXX")));
        if (!privateStage.isValid()) {
            state->result.error = privateStage.errorString();
            return;
        }
        const auto plain = privateStage.filePath(QStringLiteral("ptf"));
        state->result = PortableBackup::exportDirectory(plain, snapshot.storages, snapshot.folders, *media,
                                                         progress, keepGoing);
        if (!state->result || !keepGoing())
            return;
        QMetaObject::invokeMethod(this, [this] {
            if (!isFinished())
                setPhase(Protecting);
        }, Qt::QueuedConnection);
        const auto protectedResult = ProtectedBackup::protectDirectory(plain, directory, password, progress,
                                                                        keepGoing, beforePublish);
        if (!protectedResult) {
            state->result.error = protectedResult.error;
            state->result.complete = false;
        }
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
    password_.clear();
    StorageJob::cancel();
}

} // namespace AnyKeep
