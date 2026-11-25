#include <QtTest>

#include "backupexportjob.h"
#include "backuprestorejob.h"
#include "draftmanager.h"
#include "filedraftstore.h"
#include "filefoldercatalogstore.h"
#include "foldercatalogmanager.h"
#include "localdatakeystore.h"
#include "localmediastore.h"
#include "notemanager.h"
#include "ptfstorage.h"
#include "portablebackup.h"
#include "portablebackuparchive.h"
#include "protectedbackup.h"
#include "portablebackuprestore.h"
#include "secureenvelope.h"

#include <QCryptographicHash>
#include <QDir>
#include <QSignalSpy>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto TimeZoneUTC = QTimeZone::Initialization::UTC;
#else
constexpr auto TimeZoneUTC = Qt::UTC;
#endif

using namespace AnyKeep;

class PTFStorageTest : public QObject {
    Q_OBJECT

    static FolderCatalogSnapshot catalogWithFolder(const QUuid &folderId)
    {
        FolderRecord folder;
        folder.id         = folderId;
        folder.name       = QStringLiteral("Project");
        folder.revision   = 1;
        folder.modifiedAt = QDateTime::currentDateTimeUtc();

        FolderCatalogSnapshot snapshot;
        snapshot.folders.append(folder);
        return snapshot;
    }

private slots:
    void portableZipRoundTripAndCancellation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const QByteArray payload(2 * 1024 * 1024 + 17, 'v');
        const auto imported = media.importData(payload, QStringLiteral("video.mp4"), QStringLiteral("video/mp4"));
        QVERIFY(imported);
        Note note = source.createNote();
        note.setTitle(QStringLiteral("Portable ZIP"));
        note.setText(QStringLiteral("<video src=\"%1\"></video>").arg(imported.value.uri()), Note::Markdown);
        note.setMedia({ imported.value });
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("source");
        provider.complete = true;
        provider.notes.append({ note, QStringLiteral("original"), {} });
        const auto plain = directory.filePath(QStringLiteral("plain"));
        QVERIFY(PortableBackup::exportDirectory(plain, { provider }, {}, media));
        const auto original = PortableBackup::inspectDirectory(plain);
        QVERIFY2(original, qPrintable(original.error));

        const auto archive = directory.filePath(QStringLiteral("backup.zip"));
        QVERIFY(!PortableBackupArchive::create(plain, archive, {}, [] { return false; }));
        QVERIFY(!QFileInfo::exists(archive));
        const auto written = PortableBackupArchive::create(plain, archive);
        QVERIFY2(written, qPrintable(written.error));
        QCOMPARE(written.manifestSha256, original.manifestSha256);
        QVERIFY(!PortableBackupArchive::create(plain, archive));

        const auto restored = directory.filePath(QStringLiteral("restored"));
        QVERIFY(!PortableBackupArchive::extract(archive, restored, {}, [] { return false; }));
        QVERIFY(!QFileInfo::exists(restored));
        const auto extracted = PortableBackupArchive::extract(archive, restored);
        QVERIFY2(extracted, qPrintable(extracted.error));
        QCOMPARE(extracted.manifestSha256, original.manifestSha256);
        QVERIFY(PortableBackup::inspectDirectory(restored));
        QVERIFY(!PortableBackupArchive::extract(archive, restored));

        QFile truncated(archive);
        QVERIFY(truncated.open(QIODevice::ReadWrite));
        QVERIFY(truncated.resize(16));
        truncated.close();
        const auto invalid = directory.filePath(QStringLiteral("invalid"));
        QVERIFY(!PortableBackupArchive::extract(archive, invalid));
        QVERIFY(!QFileInfo::exists(invalid));
    }

    void protectedBackupJobsRecoverAttachmentsUnderNewProfileKey()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto privatePath = directory.filePath(QStringLiteral("private"));
        QVERIFY(QDir().mkpath(privatePath));
        const auto privateEntries = [&] {
            return QDir(privatePath).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        };
        const auto sourceKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore sourceMedia(directory.filePath(QStringLiteral("source-media")), sourceKey);
        const QByteArray payload(2 * 1024 * 1024 + 17, 'x');
        const auto imported = sourceMedia.importData(payload, QStringLiteral("private-image.png"),
                                                      QStringLiteral("image/png"));
        QVERIFY(imported);
        auto store = std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("source-drafts")), sourceKey);
        DraftRecord original;
        original.id = QUuid::createUuid();
        original.title = QStringLiteral("Private unpublished note");
        original.body = QStringLiteral("![Picture](%1)").arg(imported.value.uri());
        original.format = Note::Markdown;
        original.updatedAt = QDateTime::currentDateTimeUtc();
        original.media = { imported.value };
        QVERIFY(!store->write(original));
        DraftManager sourceDrafts(std::move(store));
        const QMap<QString, NoteStorage::Ptr> providers;
        const auto destination = directory.filePath(QStringLiteral("protected"));
        const auto password = QStringLiteral("portable recovery password");
        auto *exportJob = BackupExportJob::exportProtectedDirectory(destination, privatePath, password,
                                                                     providers, sourceDrafts, {}, sourceMedia, this);
        QSignalSpy phases(exportJob, &BackupExportJob::phaseChanged);
        QTRY_VERIFY_WITH_TIMEOUT(exportJob->isFinished(), 30000);
        QVERIFY2(exportJob->state() == StorageJob::Succeeded, qPrintable(exportJob->error().message));
        QCOMPARE(exportJob->result().notesWritten, 1);
        QVERIFY(exportJob->result().complete);
        QCOMPARE(exportJob->phase(), BackupExportJob::Protecting);
        QCOMPARE(phases.size(), 3);
        QVERIFY(privateEntries().isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(destination).filePath(QStringLiteral("manifest.json"))));
        QVERIFY(QFileInfo::exists(QDir(destination).filePath(QStringLiteral("manifest.enc"))));
        QCOMPARE(sourceDrafts.pendingDrafts().size(), 1);
        delete exportJob;

        const auto newKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore newMedia(directory.filePath(QStringLiteral("new-media")), newKey);
        DraftManager newDrafts(std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("new-drafts")), newKey));
        FolderCatalogManager folders(std::make_unique<FileFolderCatalogStore>(
            directory.filePath(QStringLiteral("new-folders")), newKey));
        QVERIFY(folders.initialize());

        auto *wrong = BackupRestoreJob::restoreProtectedDirectory(destination, QStringLiteral("wrong password"),
            privatePath, *NoteManager::instance(), newDrafts, folders, newMedia, this);
        QTRY_VERIFY_WITH_TIMEOUT(wrong->isFinished(), 30000);
        QCOMPARE(wrong->state(), StorageJob::Failed);
        QVERIFY(wrong->decisions().isEmpty());
        QVERIFY(!wrong->prepared());
        QVERIFY(newDrafts.pendingDrafts().isEmpty());
        QVERIFY(privateEntries().isEmpty());
        delete wrong;

        auto *restoreJob = BackupRestoreJob::restoreProtectedDirectory(destination, password, privatePath,
            *NoteManager::instance(), newDrafts, folders, newMedia, this);
        QTRY_VERIFY_WITH_TIMEOUT(restoreJob->phase() == BackupRestoreJob::AwaitingSelection
                                     || restoreJob->isFinished(), 30000);
        QVERIFY2(!restoreJob->isFinished(), qPrintable(restoreJob->error().message));
        QCOMPARE(restoreJob->decisions().size(), 1);
        QVERIFY(newDrafts.pendingDrafts().isEmpty());
        // Only the isolated prepared tree remains, not the decrypted
        // transport intermediate.
        QCOMPARE(privateEntries().size(), 1);
        QVERIFY(!restoreJob->apply({ { 0, BackupRestoreJob::Choice::KeepDraft, {}, {} } }, true, false));
        QTRY_VERIFY_WITH_TIMEOUT(restoreJob->isFinished(), 30000);
        QVERIFY2(restoreJob->state() == StorageJob::Succeeded, qPrintable(restoreJob->error().message));
        QCOMPARE(restoreJob->result().draftIds.size(), 1);
        const auto record = newDrafts.pendingDraft(restoreJob->result().draftIds.first());
        QVERIFY(record);
        QCOMPARE(record.value.title, original.title);
        QCOMPARE(record.value.state, DraftRecord::Editing);
        QCOMPARE(record.value.media.size(), 1);
        QVERIFY(record.value.media.first().blobId != imported.value.blobId);
        const auto restored = newMedia.data(record.value.media.first());
        QVERIFY(restored);
        QCOMPARE(restored.value, payload);
        delete restoreJob;
        QVERIFY(privateEntries().isEmpty());
    }

    void protectedBackupJobsCancelWithoutPublishingPlaintext()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto privatePath = directory.filePath(QStringLiteral("private"));
        QVERIFY(QDir().mkpath(privatePath));
        const auto key = SecureEnvelope::generateMasterKey();
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), key);
        DraftManager drafts(std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("drafts")), key));
        const QMap<QString, NoteStorage::Ptr> providers;
        const auto destination = directory.filePath(QStringLiteral("backup"));
        const auto password = QStringLiteral("portable recovery password");

        auto *job = BackupExportJob::exportProtectedDirectory(destination, privatePath, password,
                                                               providers, drafts, {}, media, this);
        connect(job, &BackupExportJob::phaseChanged, job, [job](BackupExportJob::Phase phase) {
            if (phase == BackupExportJob::Writing)
                job->cancel();
        });
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 30000);
        QCOMPARE(job->state(), StorageJob::Cancelled);
        QVERIFY(!QFileInfo::exists(destination));
        QVERIFY(QDir(privatePath).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
        delete job;

        job = BackupExportJob::exportProtectedDirectory(destination, privatePath, {}, providers, drafts, {}, media, this);
        QCOMPARE(job->state(), StorageJob::Failed);
        QVERIFY(!QFileInfo::exists(destination));
        delete job;
    }

    void protectedBackupRoundTripAndRejectsTampering()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto privatePath = directory.filePath(QStringLiteral("private"));
        QVERIFY(QDir().mkpath(privatePath));
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const QByteArray payload(2 * 1024 * 1024 + 17, 'v');
        const auto imported = media.importData(payload, QStringLiteral("private-video.mp4"),
                                               QStringLiteral("video/mp4"));
        QVERIFY(imported);
        Note note = source.createNote();
        note.setTitle(QStringLiteral("Confidential title"));
        note.setText(QStringLiteral("<video src=\"%1\"></video>").arg(imported.value.uri()), Note::Markdown);
        note.setMedia({ imported.value });
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("confidential-provider");
        provider.complete = true;
        provider.notes.append({ note, QStringLiteral("original-id"), {} });
        const auto plain = QDir(privatePath).filePath(QStringLiteral("plain"));
        QVERIFY(PortableBackup::exportDirectory(plain, { provider }, {}, media));
        const auto original = PortableBackup::inspectDirectory(plain);
        QVERIFY2(original, qPrintable(original.error));

        const auto protectedPath = directory.filePath(QStringLiteral("protected"));
        const QString password = QStringLiteral("A password for portable recovery 123");
        const auto encrypted = ProtectedBackup::protectDirectory(plain, protectedPath, password);
        QVERIFY2(encrypted, qPrintable(encrypted.error));
        QCOMPARE(encrypted.manifestSha256, original.manifestSha256);
        const QDir protectedTree(protectedPath);
        const auto files = protectedTree.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
        QCOMPARE(files.size(), original.files.size() + 2);
        QVERIFY(!files.contains(QStringLiteral("manifest.json")));
        QVERIFY(!files.contains(QStringLiteral("storage-1")));
        for (const auto &name : files) {
            QFile file(protectedTree.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            QVERIFY(!bytes.contains("Confidential title"));
            QVERIFY(!bytes.contains("private-video.mp4"));
            QVERIFY(!bytes.contains("confidential-provider"));
        }

        const auto unlocked = QDir(privatePath).filePath(QStringLiteral("unlocked"));
        QVERIFY(!ProtectedBackup::unlockDirectory(protectedPath, unlocked, QStringLiteral("wrong password")));
        QVERIFY(!QFileInfo::exists(unlocked));
        const auto decrypted = ProtectedBackup::unlockDirectory(protectedPath, unlocked, password);
        QVERIFY2(decrypted, qPrintable(decrypted.error));
        QCOMPARE(decrypted.manifestSha256, original.manifestSha256);
        QCOMPARE(decrypted.files.size(), original.files.size());
        for (qsizetype i = 0; i < original.files.size(); ++i) {
            QCOMPARE(decrypted.files.at(i).path, original.files.at(i).path);
            QCOMPARE(decrypted.files.at(i).sha256, original.files.at(i).sha256);
        }
        QVERIFY(QDir(unlocked).removeRecursively());

        // Swap two complete encrypted files. The authenticated file identity
        // and manifest inventory must reject substitution.
        QVERIFY(original.files.size() >= 2);
        const auto first = protectedTree.filePath(QStringLiteral("file-00000000.enc"));
        const auto second = protectedTree.filePath(QStringLiteral("file-00000001.enc"));
        const auto temporary = protectedTree.filePath(QStringLiteral("swap.tmp"));
        QVERIFY(QFile::rename(first, temporary));
        QVERIFY(QFile::rename(second, first));
        QVERIFY(QFile::rename(temporary, second));
        QVERIFY(!ProtectedBackup::unlockDirectory(protectedPath, unlocked, password));
        QVERIFY(!QFileInfo::exists(unlocked));
        QVERIFY(QFile::rename(first, temporary));
        QVERIFY(QFile::rename(second, first));
        QVERIFY(QFile::rename(temporary, second));

        // Change one byte without changing the file length.
        QFile manifest(protectedTree.filePath(QStringLiteral("manifest.enc")));
        QVERIFY(manifest.open(QIODevice::ReadWrite));
        QVERIFY(manifest.seek(manifest.size() - 1));
        const auto tail = manifest.read(1);
        QCOMPARE(tail.size(), 1);
        QVERIFY(manifest.seek(manifest.size() - 1));
        QCOMPARE(manifest.write(QByteArray(1, char(tail.at(0) ^ 1))), qint64(1));
        manifest.close();
        QVERIFY(!ProtectedBackup::unlockDirectory(protectedPath, unlocked, password));
        QVERIFY(!QFileInfo::exists(unlocked));
    }

    void protectedBackupHonorsCancellationAndEmptyInventory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        const auto plain = directory.filePath(QStringLiteral("plain"));
        QVERIFY(PortableBackup::exportDirectory(plain, {}, {}, media));
        const auto target = directory.filePath(QStringLiteral("protected"));
        const auto password = QStringLiteral("password for empty backup");
        QVERIFY(!ProtectedBackup::protectDirectory(plain, target, password, {}, [] { return false; }));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(!ProtectedBackup::protectDirectory(plain, target, password, {}, {}, [] { return false; }));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(ProtectedBackup::protectDirectory(plain, target, password));
        const auto unlocked = directory.filePath(QStringLiteral("unlocked"));
        QVERIFY(!ProtectedBackup::unlockDirectory(target, unlocked, password, {}, {}, [] { return false; }));
        QVERIFY(!QFileInfo::exists(unlocked));
        QVERIFY(ProtectedBackup::unlockDirectory(target, unlocked, password));
        QVERIFY(PortableBackup::inspectDirectory(unlocked));
    }

    void restoreJobWaitsForExplicitSelectionAndStagesOnlySelectedNotes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("private")));
        const auto privatePath = directory.filePath(QStringLiteral("private"));
        LocalMediaStore oldMedia(directory.filePath(QStringLiteral("old-media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(oldMedia, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const auto media = oldMedia.importData(QByteArrayLiteral("restored media"), QStringLiteral("picture.png"),
                                               QStringLiteral("image/png"));
        QVERIFY(media);
        Note first = source.createNote();
        first.setTitle(QStringLiteral("First"));
        first.setText(QStringLiteral("![Picture](%1)").arg(media.value.uri()), Note::Markdown);
        first.setMedia({ media.value });
        const auto folder = QUuid::createUuid();
        first.setFolderId(folder);
        Note second = source.createNote();
        second.setTitle(QStringLiteral("Second"));
        second.setText(QStringLiteral("Not selected"), Note::Markdown);
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("restore-job-original-unconfigured");
        provider.complete = false;
        provider.notes = { { first, QStringLiteral("first"), {} }, { second, QStringLiteral("second"), {} } };
        const auto target = directory.filePath(QStringLiteral("backup"));
        QVERIFY(PortableBackup::exportDirectory(target, { provider }, catalogWithFolder(folder), oldMedia));
        const auto preview = PortableBackup::inspectDirectory(target);
        QVERIFY2(preview, qPrintable(preview.error));
        QVERIFY(!preview.complete);

        const auto key = SecureEnvelope::generateMasterKey();
        LocalMediaStore profileMedia(directory.filePath(QStringLiteral("profile-media")), key);
        DraftManager drafts(std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("drafts")), key));
        FolderCatalogManager folders(std::make_unique<FileFolderCatalogStore>(
            directory.filePath(QStringLiteral("folders")), key));
        QVERIFY(folders.initialize());

        auto *job = BackupRestoreJob::restoreDirectory(target, preview.manifestSha256, privatePath,
                                                       *NoteManager::instance(), drafts, folders, profileMedia, this);
        QTRY_VERIFY_WITH_TIMEOUT(job->phase() == BackupRestoreJob::AwaitingSelection || job->isFinished(), 15000);
        QVERIFY2(!job->isFinished(), qPrintable(job->error().message));
        QCOMPARE(job->decisions().size(), 2);
        QCOMPARE(job->decisions().first().status, BackupRestoreDecision::ProviderMissing);
        QVERIFY(drafts.pendingDrafts().isEmpty());
        QVERIFY(!folders.catalog().folder(folder));
        const QList<BackupRestoreJob::Choice> choices {
            { 0, BackupRestoreJob::Choice::CreateCopy, {}, QStringLiteral("Recovered title") },
            { 1, BackupRestoreJob::Choice::Skip, {}, {} }
        };
        QVERIFY(job->apply(choices, true, false)); // incomplete requires explicit consent
        QVERIFY(job->apply({ choices.first() }, true, true)); // missing row
        QVERIFY(job->apply(choices, false, true)); // missing folder
        auto wrong = choices;
        wrong[0].destinationStorageId = QStringLiteral("not-configured");
        QVERIFY(job->apply(wrong, true, true));
        QCOMPARE(job->phase(), BackupRestoreJob::AwaitingSelection);
        QVERIFY(drafts.pendingDrafts().isEmpty());
        QVERIFY(!job->apply(choices, true, true));
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 15000);
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(job->result().skipped, 1);
        QCOMPARE(job->result().draftIds.size(), 1);
        QVERIFY(job->result().foldersMerged);
        QVERIFY(folders.catalog().folder(folder));
        drafts.publishPending();
        const auto record = drafts.pendingDraft(job->result().draftIds.first());
        QVERIFY(record);
        QCOMPARE(record.value.state, DraftRecord::Editing);
        QCOMPARE(record.value.title, QStringLiteral("Recovered title"));
        QCOMPARE(record.value.media.size(), 1);
        QVERIFY(record.value.remoteNoteId.isEmpty());
        const auto data = profileMedia.data(record.value.media.first());
        QVERIFY(data);
        QCOMPARE(data.value, QByteArrayLiteral("restored media"));
        delete job;
        QVERIFY(QDir(privatePath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void restoreJobCancellationBeforeCommitLeavesProfileUntouched()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("private")));
        const auto privatePath = directory.filePath(QStringLiteral("private"));
        const auto key = SecureEnvelope::generateMasterKey();
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), key);
        const auto target = directory.filePath(QStringLiteral("backup"));
        QVERIFY(PortableBackup::exportDirectory(target, {}, {}, media));
        const auto preview = PortableBackup::inspectDirectory(target);
        QVERIFY(preview);
        DraftManager drafts(std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("drafts")), key));
        FolderCatalogManager folders(std::make_unique<FileFolderCatalogStore>(
            directory.filePath(QStringLiteral("folders")), key));
        QVERIFY(folders.initialize());
        auto *job = BackupRestoreJob::restoreDirectory(target, preview.manifestSha256, privatePath,
                                                       *NoteManager::instance(), drafts, folders, media, this);
        job->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 15000);
        QCOMPARE(job->state(), StorageJob::Cancelled);
        QVERIFY(drafts.pendingDrafts().isEmpty());
        delete job;
        QVERIFY(QDir(privatePath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void prepareBackupRestoresMediaInIsolationAndKeepsMissingIdentity()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("private")));
        const auto privatePath = directory.filePath(QStringLiteral("private"));
        LocalMediaStore oldMedia(directory.filePath(QStringLiteral("old-media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(oldMedia, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const auto present = oldMedia.importData(QByteArrayLiteral("real image"), QStringLiteral("same.png"),
                                                  QStringLiteral("image/png"));
        QVERIFY(present);
        MediaReference missing;
        missing.id = QUuid::createUuid();
        missing.originalName = QStringLiteral("same.png");
        missing.portableName = missing.originalName;
        missing.mediaType = QStringLiteral("image/png");
        missing.size = 123;
        missing.checksum = QCryptographicHash::hash(QByteArrayLiteral("different image"), QCryptographicHash::Sha256);
        const auto folder = QUuid::createUuid();
        Note note = source.createNote();
        note.setTitle(QStringLiteral("Restored note"));
        note.setText(QStringLiteral("![Present](%1)\n\n![Missing](%2)").arg(present.value.uri(), missing.uri()),
                     Note::Markdown);
        note.setMedia({ present.value, missing });
        note.setFolderId(folder);
        note.setFavorite(true);
        const auto modified = QDateTime::currentDateTimeUtc();
        note.setLastChangeUTC(modified);
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("original-provider");
        provider.complete = true;
        provider.notes.append({ note, QStringLiteral("opaque-note-id"), {} });

        const auto target = directory.filePath(QStringLiteral("backup"));
        QVERIFY(PortableBackup::exportDirectory(target, { provider }, catalogWithFolder(folder), oldMedia));
        const auto preview = PortableBackup::inspectDirectory(target);
        QVERIFY2(preview, qPrintable(preview.error));
        QVERIFY(!preview.complete);
        QCOMPARE(preview.unavailableMedia, 1);
        QString error;
        auto restored = PortableBackupRestore::prepareDirectory(target, preview.manifestSha256, privatePath, &error);
        QVERIFY2(restored, qPrintable(error));
        QVERIFY(!restored->inspection().complete);
        QCOMPARE(restored->storages().size(), 1);
        const auto &entry = restored->storages().first().notes.first();
        QCOMPARE(entry.sourceNoteId, QStringLiteral("opaque-note-id"));
        QCOMPARE(entry.snapshot.id(), entry.sourceNoteId);
        QVERIFY(entry.snapshot.storage() == nullptr);
        QCOMPARE(entry.snapshot.folderId(), folder);
        QVERIFY(entry.snapshot.isFavorite());
        QCOMPARE(entry.snapshot.lastChangeUTC(), modified);
        QCOMPARE(entry.snapshot.media().size(), 2);
        QVERIFY(entry.snapshot.text().contains(missing.uri()));
        bool foundPresent = false;
        bool foundMissing = false;
        for (const auto &reference : entry.snapshot.media()) {
            if (reference.id == present.value.id) {
                foundPresent = true;
                QVERIFY(reference.blobId != present.value.blobId);
                const auto data = restored->mediaStore().data(reference);
                QVERIFY(data);
                QCOMPARE(data.value, QByteArrayLiteral("real image"));
            } else if (reference.id == missing.id) {
                foundMissing = true;
                QVERIFY(reference.blobId.isEmpty());
                QCOMPARE(reference.checksum, missing.checksum);
                QCOMPARE(reference.uri(), missing.uri());
                QVERIFY(!restored->mediaStore().contains(reference));
            }
        }
        QVERIFY(foundPresent && foundMissing);

        // Recovery copies are encrypted under the actual destination key,
        // not the key of the temporary preparation or the old installation.
        const auto profileKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore profileMedia(directory.filePath(QStringLiteral("profile-media")), profileKey);
        const auto copies = restored->prepareCopies({ { 0, 0, {} } }, profileMedia);
        QVERIFY2(copies, qPrintable(copies.error.message));
        QCOMPARE(copies.value.size(), 1);
        const auto &copy = copies.value.first().snapshot;
        QCOMPARE(copy.media().size(), 2);
        QVERIFY(copy.id().isEmpty());
        for (const auto &reference : copy.media()) {
            QVERIFY(reference.id != present.value.id && reference.id != missing.id);
            QVERIFY(copy.text().contains(reference.uri()));
            if (reference.blobId.isEmpty()) {
                QCOMPARE(reference.checksum, missing.checksum);
                QVERIFY(!profileMedia.contains(reference));
            } else {
                const auto data = profileMedia.data(reference);
                QVERIFY(data);
                QCOMPARE(data.value, QByteArrayLiteral("real image"));
            }
        }
        QVERIFY(!copy.text().contains(missing.uri()));

        FolderCatalogManager folders(std::make_unique<FileFolderCatalogStore>(
            directory.filePath(QStringLiteral("profile-folders")), profileKey));
        QVERIFY(folders.initialize());
        QVERIFY(!folders.mergeRecoveredFolders(restored->folders()));
        QVERIFY(folders.catalog().folder(folder));
        QVERIFY(!folders.renameFolder(folder, QStringLiteral("Current folder")));
        QVERIFY(!folders.mergeRecoveredFolders(restored->folders()));
        QCOMPARE(folders.catalog().folder(folder)->name, QStringLiteral("Current folder"));

        DraftManager drafts(std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("profile-drafts")),
                                                             profileKey));
        const auto staged = drafts.stageRecoveredCopies(copies.value);
        QVERIFY2(staged, qPrintable(staged.error.message));
        QCOMPARE(staged.value.size(), 1);
        drafts.publishPending();
        const auto draft = drafts.pendingDraft(staged.value.first());
        QVERIFY(draft);
        QCOMPARE(draft.value.state, DraftRecord::Editing);
        QVERIFY(draft.value.storageId.isEmpty());
        QVERIFY(draft.value.remoteNoteId.isEmpty());
        QVERIFY(draft.value.removeSourceNoteId.isEmpty());
        QCOMPARE(draft.value.folderId, folder);
        QVERIFY(draft.value.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool());
        QCOMPARE(draft.value.media, copy.media());

        QVERIFY(QFileInfo::exists(target));
        QVERIFY(!QDir(privatePath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        restored.reset();
        QVERIFY(QDir(privatePath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

        auto wrong = PortableBackupRestore::prepareDirectory(target, QByteArray(32, 'x'), privatePath, &error);
        QVERIFY(!wrong);
        QVERIFY(!error.isEmpty());
        auto cancelled = PortableBackupRestore::prepareDirectory(
            target, preview.manifestSha256, privatePath, &error, [](int, int) { return false; });
        QVERIFY(!cancelled);
        QVERIFY(QDir(privatePath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }

    void portableBackupAvoidsCaseAliasesAndReservedSidecarNames()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const auto first = media.importData(QByteArrayLiteral("one"), QStringLiteral("Photo.png"),
                                            QStringLiteral("image/png"));
        const auto second = media.importData(QByteArrayLiteral("two"), QStringLiteral("photo.png"),
                                             QStringLiteral("image/png"));
        const auto reserved = media.importData(QByteArrayLiteral("three"), QStringLiteral(".anykeep-media.json"),
                                               QStringLiteral("application/json"));
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(reserved);
        Note note = source.createNote();
        note.setTitle(QStringLiteral("Report"));
        note.setText(QStringLiteral("![A](%1)\n\n![B](%2)\n\n<a href=\"%3\" data-anykeep-attachment=\"1\">File</a>")
                         .arg(first.value.uri(), second.value.uri(), reserved.value.uri()), Note::Markdown);
        note.setMedia({ first.value, second.value, reserved.value });
        Note other = source.createNote();
        other.setTitle(QStringLiteral("report"));
        other.setText(QStringLiteral("Another note"), Note::Markdown);
        PortableBackupStorage provider;
        provider.storageId = source.systemName();
        provider.complete = true;
        provider.notes.append({ note, QStringLiteral("first"), {} });
        provider.notes.append({ other, QStringLiteral("second"), {} });
        const auto target = directory.filePath(QStringLiteral("backup"));
        QVERIFY(PortableBackup::exportDirectory(target, { provider }, {}, media));
        const auto preview = PortableBackup::inspectDirectory(target);
        QVERIFY2(preview, qPrintable(preview.error));
        QCOMPARE(preview.notesWritten, 2);
        const auto &notes = preview.storages.first().notes;
        QVERIFY(notes[0].ptfId.compare(notes[1].ptfId, Qt::CaseInsensitive) != 0);

        LocalMediaStore fresh(directory.filePath(QStringLiteral("fresh-media")), SecureEnvelope::generateMasterKey());
        PTFStorage restored(fresh, QDir(target).filePath(preview.storages.first().directory));
        QVERIFY(restored.init());
        const auto imported = restored.note(notes.first().ptfId);
        QCOMPARE(imported.media().size(), 3);
        QSet<QByteArray> payloads;
        for (const auto &reference : imported.media()) {
            const auto data = fresh.data(reference);
            QVERIFY(data);
            payloads.insert(data.value);
        }
        QCOMPARE(payloads, (QSet<QByteArray> { QByteArrayLiteral("one"), QByteArrayLiteral("two"),
                                               QByteArrayLiteral("three") }));
    }

    void backupInspectionVerifiesInventoryAndOrigin()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const auto image = media.importData(QByteArrayLiteral("image bytes"), QStringLiteral("photo.png"),
                                            QStringLiteral("image/png"));
        QVERIFY(image);
        Note note = source.createNote();
        note.setTitle(QStringLiteral("A note"));
        note.setText(QStringLiteral("![Picture](%1)").arg(image.value.uri()), Note::Markdown);
        note.setMedia({ image.value });
        const auto originalDate = QDateTime::currentDateTimeUtc();
        note.setLastChangeUTC(originalDate);
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("original-provider");
        provider.displayName = QStringLiteral("Provider");
        provider.complete = true;
        provider.remote = true;
        provider.notes.append({ note, QStringLiteral("opaque/id"), {} });

        PortableBackupStorage drafts;
        drafts.storageId = DraftManager::draftsStorageId();
        drafts.displayName = QStringLiteral("Drafts");
        drafts.complete = true;
        PortableBackupDraftInfo pending;
        pending.id = QUuid::createUuid();
        pending.storageId = provider.storageId;
        pending.noteId = QStringLiteral("opaque/id");
        pending.state = DraftRecord::Retry;
        pending.revision = 3;
        pending.updatedAt = originalDate;
        drafts.drafts.append(pending);
        drafts.notes.append({ note, pending.noteId, pending.id });
        PortableBackupDraftInfo deletion;
        deletion.id = QUuid::createUuid();
        deletion.operation = DraftRecord::Delete;
        deletion.storageId = provider.storageId;
        deletion.noteId = QStringLiteral("another/id");
        drafts.drafts.append(deletion);

        const auto target = directory.filePath(QStringLiteral("backup"));
        QVERIFY(PortableBackup::exportDirectory(target, { provider, drafts }, {}, media));
        const auto inspected = PortableBackup::inspectDirectory(target);
        QVERIFY2(inspected, qPrintable(inspected.error));
        QVERIFY(inspected.complete);
        QCOMPARE(inspected.notesWritten, 2);
        QCOMPARE(inspected.storages.size(), 2);
        QCOMPARE(inspected.storages.first().storageId, provider.storageId);
        QCOMPARE(inspected.storages.first().notes.first().sourceNoteId, QStringLiteral("opaque/id"));
        QCOMPARE(inspected.storages.first().notes.first().modifiedUtc, originalDate);
        QCOMPARE(inspected.storages.last().notes.first().draftId, pending.id);
        QCOMPARE(inspected.storages.last().drafts.size(), 2);
        QCOMPARE(inspected.storages.last().drafts.first().revision, quint64(3));
        QCOMPARE(inspected.storages.last().drafts.last().operation, DraftRecord::Delete);
        QCOMPARE(inspected.manifestSha256.size(), 32);

        QString attachmentPath;
        for (const auto &file : inspected.files) {
            if (file.path.endsWith(QStringLiteral("/photo.png"))) {
                attachmentPath = QDir(target).filePath(file.path);
                break;
            }
        }
        QVERIFY(!attachmentPath.isEmpty());
        QFile attachment(attachmentPath);
        QVERIFY(attachment.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(attachment.write("wrong bytes"), qint64(11)); // same size as original
        attachment.close();
        const auto damaged = PortableBackup::inspectDirectory(target);
        QVERIFY(!damaged);
        QVERIFY(damaged.error.contains(QStringLiteral("integrity")));
    }

    void backupInspectionRejectsMissingExtraAndUnsafeEntries()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        Note note = source.createNote();
        note.setTitle(QStringLiteral("Stored"));
        note.setText(QStringLiteral("Body"), Note::Markdown);
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("provider");
        provider.complete = false;
        provider.omittedNoteIds = { QStringLiteral("unavailable-note") };
        provider.notes.append({ note, QStringLiteral("id"), {} });
        const auto target = directory.filePath(QStringLiteral("backup"));
        QVERIFY(PortableBackup::exportDirectory(target, { provider }, {}, media));
        const auto preview = PortableBackup::inspectDirectory(target);
        QVERIFY2(preview, qPrintable(preview.error));
        QVERIFY(!preview.complete);
        QCOMPARE(preview.storages.first().omittedNoteIds, provider.omittedNoteIds);

        const auto manifestPath = QDir(target).filePath(QStringLiteral("manifest.json"));
        QFile file(manifestPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto originalBytes = file.readAll();
        file.close();
        const auto originalManifest = QJsonDocument::fromJson(originalBytes).object();
        const auto writeManifest = [&](const QJsonObject &object) {
            QFile output(manifestPath);
            QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
            const auto bytes = QJsonDocument(object).toJson();
            QCOMPARE(output.write(bytes), qint64(bytes.size()));
        };

        auto invalid = originalManifest;
        invalid.insert(QStringLiteral("complete"), true);
        writeManifest(invalid);
        QVERIFY(!PortableBackup::inspectDirectory(target));

        invalid = originalManifest;
        auto files = invalid.value(QStringLiteral("files")).toArray();
        auto first = files.first().toObject();
        first.insert(QStringLiteral("path"), QStringLiteral("storage-1/../outside.md"));
        files.replace(0, first);
        invalid.insert(QStringLiteral("files"), files);
        writeManifest(invalid);
        QVERIFY(!PortableBackup::inspectDirectory(target));

        invalid = originalManifest;
        files = invalid.value(QStringLiteral("files")).toArray();
        files.append(files.first());
        invalid.insert(QStringLiteral("files"), files);
        writeManifest(invalid);
        QVERIFY(!PortableBackup::inspectDirectory(target));

        writeManifest(originalManifest);
        QFile extra(QDir(target).filePath(QStringLiteral("extra.md")));
        QVERIFY(extra.open(QIODevice::WriteOnly));
        extra.close();
        QVERIFY(!PortableBackup::inspectDirectory(target));
        QVERIFY(extra.remove());

#ifndef Q_OS_WIN
        const auto extraLink = QDir(target).filePath(QStringLiteral("external-link"));
        QVERIFY(QFile::link(manifestPath, extraLink));
        QVERIFY(!PortableBackup::inspectDirectory(target));
        QVERIFY(QFile::remove(extraLink));
#endif
        QVERIFY(PortableBackup::inspectDirectory(target));
        bool checked = false;
        const auto cancelled = PortableBackup::inspectDirectory(target, [&checked](int, int) {
            checked = true;
            return false;
        });
        QVERIFY(checked);
        QVERIFY(!cancelled);

        const auto &noteInfo = preview.storages.first().notes.first();
        QVERIFY(QFile::remove(QDir(target).filePath(QStringLiteral("storage-1/") + noteInfo.ptfId
                                                    + QStringLiteral(".md"))));
        QVERIFY(!PortableBackup::inspectDirectory(target));
    }

    void asynchronousBackupWritesProviderAndDraftMediaWithInventory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto key = SecureEnvelope::generateMasterKey();
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), key);
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const QByteArray imageBytes("image payload");
        const auto image = media.importData(imageBytes, QStringLiteral("diagram.png"), QStringLiteral("image/png"));
        QVERIFY(image);

        Note note = source.createNote();
        note.setTitle(QStringLiteral("Published note"));
        note.setText(QStringLiteral("![Picture](%1)").arg(image.value.uri()), Note::Markdown);
        note.setMedia({ image.value });
        QVERIFY(source.saveNote(note));
        const auto published = source.noteList().constFirst();
        QVERIFY(!published.id().isEmpty());

        auto store = std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("drafts")), key);
        DraftRecord record;
        record.id = QUuid::createUuid();
        record.storageId = source.systemName();
        record.remoteNoteId = published.id();
        record.title = QStringLiteral("Unpublished edit");
        record.body = QStringLiteral("New body with ![Picture](%1)").arg(image.value.uri());
        record.format = Note::Markdown;
        record.media = { image.value };
        record.updatedAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!store->write(record));
        DraftManager drafts(std::move(store));

        const QMap<QString, NoteStorage::Ptr> providers { { source.systemName(), &source } };
        const auto destination = directory.filePath(QStringLiteral("backup"));
        auto *job = BackupExportJob::exportDirectory(destination, providers, drafts, {}, media, this);
        QSignalSpy phases(job, &BackupExportJob::phaseChanged);
        QSignalSpy finished(job, &StorageJob::finished);
        QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 15000);
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QVERIFY2(job->result(), qPrintable(job->result().error));
        QVERIFY(job->result().complete);
        QCOMPARE(job->result().notesWritten, 2);
        QCOMPARE(job->result().unavailableMedia, 0);
        QCOMPARE(finished.size(), 1);
        QCOMPARE(phases.size(), 2);
        QCOMPARE(drafts.pendingDrafts().size(), 1);
        QCOMPARE(drafts.pendingDrafts().constFirst().state, DraftRecord::Editing);

        QFile manifest(QDir(destination).filePath(QStringLiteral("manifest.json")));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(manifest.readAll()).object();
        const auto files = root.value(QStringLiteral("files")).toArray();
        QVERIFY(!files.isEmpty());
        int matchedImages = 0;
        bool hasSidecar = false;
        for (const auto &entry : files) {
            const auto item = entry.toObject();
            const auto path = item.value(QStringLiteral("path")).toString();
            QFile file(QDir(destination).filePath(path));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto bytes = file.readAll();
            QCOMPARE(item.value(QStringLiteral("size")).toString(), QString::number(bytes.size()));
            QCOMPARE(item.value(QStringLiteral("sha256")).toString(),
                     QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
            if (path.endsWith(QStringLiteral("/diagram.png"))) {
                QCOMPARE(bytes, imageBytes);
                ++matchedImages;
            }
            if (path.endsWith(QStringLiteral("/.anykeep-media.json")))
                hasSidecar = true;
        }
        QCOMPARE(matchedImages, 2);
        QVERIFY(hasSidecar);
        delete job;
    }

    void asynchronousBackupCancellationBeforeWritingAndAtPublishGate()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto key = SecureEnvelope::generateMasterKey();
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), key);
        DraftManager drafts(std::make_unique<FileDraftStore>(directory.filePath(QStringLiteral("drafts")), key));
        const QMap<QString, NoteStorage::Ptr> providers;
        const auto earlyPath = directory.filePath(QStringLiteral("early"));
        auto *early = BackupExportJob::exportDirectory(earlyPath, providers, drafts, {}, media, this);
        early->cancel();
        QCOMPARE(early->state(), StorageJob::Cancelled);
        QCoreApplication::processEvents();
        QVERIFY(!QFileInfo::exists(earlyPath));
        delete early;

        const auto writingPath = directory.filePath(QStringLiteral("writing"));
        auto *writing = BackupExportJob::exportDirectory(writingPath, providers, drafts, {}, media, this);
        connect(writing, &BackupExportJob::phaseChanged, writing, [writing](BackupExportJob::Phase phase) {
            if (phase == BackupExportJob::Writing)
                writing->cancel();
        });
        QTRY_VERIFY(writing->isFinished());
        QCOMPARE(writing->state(), StorageJob::Cancelled);
        QVERIFY(!QFileInfo::exists(writingPath));
        delete writing;

        const auto gatePath = directory.filePath(QStringLiteral("gate"));
        bool gateCalled = false;
        const auto rejected = PortableBackup::exportDirectory(gatePath, {}, {}, media, {}, {},
                                                               [&gateCalled] {
                                                                   gateCalled = true;
                                                                   return false;
                                                               });
        QVERIFY(gateCalled);
        QVERIFY(!rejected);
        QVERIFY(!QFileInfo::exists(gatePath));
    }

    void portableExportCanCancelDuringAttachmentCopy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage source(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(source.init());
        const QByteArray bytes(3 * 1024 * 1024, 'a');
        const auto attachment = media.importData(bytes, QStringLiteral("large.dat"),
                                                  QStringLiteral("application/octet-stream"));
        QVERIFY(attachment);
        Note note = source.createNote();
        note.setTitle(QStringLiteral("Large attachment"));
        note.setText(QStringLiteral("<a href=\"%1\" data-anykeep-attachment=\"1\">File</a>")
                         .arg(attachment.value.uri()), Note::Markdown);
        note.setMedia({ attachment.value });
        PortableBackupStorage provider;
        provider.storageId = source.systemName();
        provider.complete = true;
        provider.notes.append({ note, QStringLiteral("note"), {} });
        const auto target = directory.filePath(QStringLiteral("cancelled-media"));
        int checks = 0;
        // Initial, storage and note checks, then attachment and chunk checks.
        const auto result = PortableBackup::exportDirectory(target, { provider }, {}, media, {},
                                                             [&checks] { return ++checks < 6; });
        QVERIFY(!result);
        QCOMPARE(checks, 7); // cancellation is also checked when copy returns
        QVERIFY(!QFileInfo::exists(target));
    }

    void portableExportUsesDetachedPtfAndRestoresWithNewKey()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QVariant configuredPath = QSettings().value(QStringLiteral("storage.ptf.path"));
        LocalMediaStore oldMedia(directory.filePath(QStringLiteral("old-media")), SecureEnvelope::generateMasterKey());
        PTFStorage sourceStorage(oldMedia, directory.filePath(QStringLiteral("source")));
        QVERIFY(sourceStorage.init());
        QVERIFY(!sourceStorage.isConfigurable());
        QVERIFY(!sourceStorage.setStoragePath(directory.path()));
        QCOMPARE(sourceStorage.customStoragePath(), directory.filePath(QStringLiteral("source")));

        const auto image = oldMedia.importData(QByteArrayLiteral("picture"), QStringLiteral("diagram.png"),
                                               QStringLiteral("image/png"));
        const auto video = oldMedia.importData(QByteArrayLiteral("video bytes"), QStringLiteral("clip.mp4"),
                                               QStringLiteral("video/mp4"));
        QVERIFY(image);
        QVERIFY(video);
        QFile external(directory.filePath(QStringLiteral("document.pdf")));
        QVERIFY(external.open(QIODevice::WriteOnly));
        QCOMPARE(external.write("external document"), qint64(17));
        external.close();
        const auto linked = oldMedia.referenceFile(external.fileName());
        QVERIFY(linked);
        QVERIFY(linked.value.blobId.isEmpty());

        const QUuid folderId = QUuid::createUuid();
        Note source = sourceStorage.createNote();
        source.setTitle(QStringLiteral("Portable note"));
        source.setText(QStringLiteral("![Diagram](%1)\n\n<video src=\"%2\" title=\"Clip\"></video>\n\n"
                                      "<a href=\"%3\" data-anykeep-attachment=\"1\">Document</a>")
                           .arg(image.value.uri(), video.value.uri(), linked.value.uri()),
                       Note::Markdown);
        source.setMedia({ image.value, video.value, linked.value });
        source.setFavorite(true);
        source.setFolderId(folderId);

        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("remote-instance-1");
        provider.displayName = QStringLiteral("Example");
        provider.remote = true;
        provider.complete = true;
        provider.notes.append({ source, QStringLiteral("server-note-42"), {} });

        const auto backupPath = directory.filePath(QStringLiteral("backup"));
        const auto result = PortableBackup::exportDirectory(backupPath, { provider }, catalogWithFolder(folderId),
                                                            oldMedia);
        QVERIFY2(result, qPrintable(result.error));
        QVERIFY(result.complete);
        QCOMPARE(result.notesWritten, 1);
        QCOMPARE(result.unavailableMedia, 0);
        QCOMPARE(QSettings().value(QStringLiteral("storage.ptf.path")), configuredPath);
        QVERIFY(sourceStorage.noteList().isEmpty());

        QFile manifest(QDir(backupPath).filePath(QStringLiteral("manifest.json")));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(manifest.readAll()).object();
        const auto storage = root.value(QStringLiteral("storages")).toArray().at(0).toObject();
        QCOMPARE(storage.value(QStringLiteral("storageId")).toString(), provider.storageId);
        const auto exported = storage.value(QStringLiteral("notes")).toArray().at(0).toObject();
        QCOMPARE(exported.value(QStringLiteral("sourceNoteId")).toString(), QStringLiteral("server-note-42"));

        // The exported files are independent of the old local encryption key
        // and the original external file.
        QVERIFY(external.remove());
        LocalMediaStore freshMedia(directory.filePath(QStringLiteral("fresh-media")),
                                   SecureEnvelope::generateMasterKey());
        PTFStorage restored(freshMedia, QDir(backupPath).filePath(storage.value(QStringLiteral("directory")).toString()));
        QVERIFY(restored.init());
        const auto savedId = exported.value(QStringLiteral("ptfId")).toString();
        const Note loaded = restored.note(savedId);
        QVERIFY(!loaded.isNull());
        QCOMPARE(loaded.folderId(), folderId);
        QVERIFY(loaded.isFavorite());
        QCOMPARE(loaded.media().size(), 3);
        const auto media = loaded.media();
        const auto check = [&](const QUuid &id, const QByteArray &expected) {
            const auto it = std::find_if(media.cbegin(), media.cend(),
                                         [&id](const MediaReference &ref) { return ref.id == id; });
            QVERIFY(it != media.cend());
            const auto data = freshMedia.data(*it);
            QVERIFY2(data, qPrintable(data.error));
            QCOMPARE(data.value, expected);
        };
        check(image.value.id, QByteArrayLiteral("picture"));
        check(video.value.id, QByteArrayLiteral("video bytes"));
        check(linked.value.id, QByteArrayLiteral("external document"));
        QCOMPARE(QSettings().value(QStringLiteral("storage.ptf.path")), configuredPath);
    }

    void portableExportReportsUnavailableMediaAndDoesNotPublishCancelledOutput()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore media(directory.filePath(QStringLiteral("media")), SecureEnvelope::generateMasterKey());
        PTFStorage sourceStorage(media, directory.filePath(QStringLiteral("source")));
        QVERIFY(sourceStorage.init());
        MediaReference missing;
        missing.id = QUuid::createUuid();
        missing.portableName = QStringLiteral("missing.pdf");
        missing.originalName = missing.portableName;
        missing.mediaType = QStringLiteral("application/pdf");
        missing.size = 100;
        Note source = sourceStorage.createNote();
        source.setTitle(QStringLiteral("Offline"));
        source.setText(QStringLiteral("<a href=\"%1\" data-anykeep-attachment=\"1\">Document</a>").arg(missing.uri()),
                       Note::Markdown);
        source.setMedia({ missing });
        PortableBackupStorage provider;
        provider.storageId = QStringLiteral("offline-storage");
        provider.remote = true;
        provider.complete = false;
        provider.notes.append({ source, QStringLiteral("id-1"), QUuid::createUuid() });

        const auto backupPath = directory.filePath(QStringLiteral("backup"));
        auto result = PortableBackup::exportDirectory(backupPath, { provider }, {}, media);
        QVERIFY2(result, qPrintable(result.error));
        QVERIFY(!result.complete);
        QCOMPARE(result.unavailableMedia, 1);
        QFile manifest(QDir(backupPath).filePath(QStringLiteral("manifest.json")));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(manifest.readAll()).object();
        QVERIFY(!root.value(QStringLiteral("complete")).toBool());
        const auto storage = root.value(QStringLiteral("storages")).toArray().at(0).toObject();
        const auto note = storage.value(QStringLiteral("notes")).toArray().at(0).toObject();
        QCOMPARE(note.value(QStringLiteral("draftId")).toString(),
                 provider.notes.first().draftId.toString(QUuid::WithoutBraces));
        const auto unavailable = note.value(QStringLiteral("unavailableMedia")).toArray();
        QCOMPARE(unavailable.size(), 1);
        QCOMPARE(unavailable.at(0).toObject().value(QStringLiteral("uri")).toString(), missing.uri());
        QFile raw(QDir(backupPath).filePath(storage.value(QStringLiteral("directory")).toString() + QLatin1Char('/')
                                          + note.value(QStringLiteral("ptfId")).toString() + QStringLiteral(".md")));
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QVERIFY(raw.readAll().contains(missing.uri().toUtf8()));

        const auto cancelledPath = directory.filePath(QStringLiteral("cancelled"));
        result = PortableBackup::exportDirectory(cancelledPath, { provider }, {}, media,
                                                 [](int done, int) { return done == 0; });
        QVERIFY(!result);
        QVERIFY(!QFileInfo::exists(cancelledPath));
        QVERIFY(!PortableBackup::exportDirectory(backupPath, { provider }, {}, media));
    }

    void codeOnlyNoteKeepsEmptyTitleAndExactBody()
    {
        QTemporaryDir notesDirectory;
        QVERIFY(notesDirectory.isValid());

        const QString body = QStringLiteral("```qml\nItem {\n    property int value: 1\n}\n```");
        PTFStorage    writer;
        QVERIFY(writer.setStoragePath(notesDirectory.path()));
        Note note = writer.createNote();
        note.setTitle(QString());
        note.setText(body, Note::Markdown);
        QVERIFY(writer.saveNote(note));

        const auto summaries = writer.noteList();
        QCOMPARE(summaries.size(), 1);
        QCOMPARE(summaries.constFirst().title(), QString());
        QCOMPARE(summaries.constFirst().displayTitle(), QStringLiteral("QML code"));
        const QString fileName = summaries.constFirst().backendValue(QStringLiteral("fileName")).toString();
        QCOMPARE(QFileInfo(fileName).completeBaseName(), QStringLiteral("QML code"));
        QFile file(fileName);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), body);

        Note loaded = summaries.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.title(), QString());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.displayTitle(), QStringLiteral("QML code"));
    }

    void structuralMarkdownStartsStayInTheBody_data()
    {
        QTest::addColumn<QString>("firstBlock");
        QTest::newRow("image") << QStringLiteral("![Diagram](https://example.org/diagram.png)");
        QTest::newRow("styled image")
            << QStringLiteral("<p align=\"right\"><img src=\"https://example.org/diagram.png\" alt=\"Diagram\" width=\"320\" /></p>");
        QTest::newRow("audio")
            << QStringLiteral("<audio controls src=\"https://example.org/recording.m4a\" title=\"Voice memo\"></audio>");
        QTest::newRow("video")
            << QStringLiteral("<video src=\"https://example.org/movie.mp4\" title=\"Clip\"></video>");
        QTest::newRow("attachment")
            << QStringLiteral("<a href=\"https://example.org/file.pdf\" data-anykeep-attachment=\"1\" "
                              "data-anykeep-media-type=\"application/pdf\" data-anykeep-size=\"50\">file.pdf</a>");
    }

    void structuralMarkdownStartsStayInTheBody()
    {
        QFETCH(QString, firstBlock);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));

        Note note = storage.createNote();
        note.setTitle(QString());
        note.setText(QStringLiteral("\n\n") + firstBlock, Note::Markdown);
        QVERIFY(storage.saveNote(note));
        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.constFirst().title(), QString());
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.title(), QString());
        QCOMPARE(loaded.text(), firstBlock);
    }

    void leadingBlankMarkdownHeadingIsCanonicalPtfTitle()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString heading = QStringLiteral("# Открытые задачи и эпики по `component = Platform_HWS`");
        const QString expected = QStringLiteral("Открытые задачи и эпики по component = Platform_HWS");
        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));

        Note note = storage.createNote();
        note.setTitle(QString());
        note.setText(QStringLiteral("\n\n") + heading + QStringLiteral("\n\nBody"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        const Note summary = notes.constFirst();
        QCOMPARE(summary.title(), heading);
        QCOMPARE(summary.displayTitle(), expected);

        const auto fileName = summary.backendValue(QStringLiteral("fileName")).toString();
        QVERIFY(QFileInfo(fileName).completeBaseName().contains(QStringLiteral("Platform_HWS")));
        QVERIFY(!QFileInfo(fileName).completeBaseName().contains(QLatin1Char('`')));
        QFile file(fileName);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString stored = QString::fromUtf8(file.readAll());
        QVERIFY(stored.startsWith(heading + QStringLiteral("\n\nBody")));
        QVERIFY(!stored.startsWith(QLatin1Char('\n')));

        Note loaded = summary;
        QVERIFY(loaded.load());
        QCOMPARE(loaded.title(), heading);
        QCOMPARE(loaded.displayTitle(), expected);
        QCOMPARE(loaded.text(), QStringLiteral("\nBody"));
    }

    void legacyPtfLeadingBlankLinesAreSkippedWhenLoading()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString heading = QStringLiteral("# Legacy `inline code`");
        QFile raw(directory.filePath(QStringLiteral("legacy.md")));
        QVERIFY(raw.open(QIODevice::WriteOnly));
        const QByteArray contents = (QStringLiteral("\n\n") + heading + QStringLiteral("\n\nBody")).toUtf8();
        QCOMPARE(raw.write(contents), qint64(contents.size()));
        raw.close();

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.constFirst().title(), heading);
        QCOMPARE(notes.constFirst().displayTitle(), QStringLiteral("Legacy inline code"));
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.title(), heading);
        QCOMPARE(loaded.displayTitle(), QStringLiteral("Legacy inline code"));
    }

    void styledAndMarkdownImagesSurviveFreshStorageInstance()
    {
        QTemporaryDir notesDirectory;
        QTemporaryDir mediaDirectory;
        QVERIFY(notesDirectory.isValid());
        QVERIFY(mediaDirectory.isValid());

        const auto masterKey = SecureEnvelope::generateMasterKey();
        QCOMPARE(masterKey.size(), LocalDataKeyStore::MasterKeySize);
        LocalMediaStore  mediaWriter(mediaDirectory.path(), masterKey);
        const QByteArray imageBytes("not-decoded-by-the-storage-test");
        const auto       imported = mediaWriter.importData(imageBytes, QStringLiteral("diagram with spaces.png"),
                                                           QStringLiteral("image/png"));
        QVERIFY2(imported, qPrintable(imported.error));

        QString body = QStringLiteral("![Default](%1)\n\n"
                                      "<p align=\"right\"><img src=\"%1\" alt=\"Styled\" width=\"320\" /></p>")
                           .arg(imported.value.uri());
        {
            PTFStorage writer(mediaWriter);
            QVERIFY(writer.setStoragePath(notesDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Images"));
            note.setText(body, Note::Markdown);
            note.setMedia({ imported.value });
            QVERIFY(writer.saveNote(note));
        }

        LocalMediaStore mediaReader(mediaDirectory.path(), masterKey);
        PTFStorage      reader(mediaReader);
        QVERIFY(reader.setStoragePath(notesDirectory.path()));
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.media().size(), 1);
        QCOMPARE(loaded.media().constFirst().id, imported.value.id);
        const auto opened = mediaReader.data(loaded.media().constFirst().blobId);
        QVERIFY2(opened, qPrintable(opened.error));
        QCOMPARE(opened.value, imageBytes);
    }

    void audioAttachmentSurvivesFreshStorageInstance()
    {
        QTemporaryDir notesDirectory;
        QTemporaryDir mediaDirectory;
        QVERIFY(notesDirectory.isValid());
        QVERIFY(mediaDirectory.isValid());

        const auto       masterKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore  mediaWriter(mediaDirectory.path(), masterKey);
        const QByteArray audioBytes("encoded-audio-payload");
        const auto       imported
            = mediaWriter.importData(audioBytes, QStringLiteral("recording.m4a"), QStringLiteral("audio/mp4"));
        QVERIFY2(imported, qPrintable(imported.error));

        const QString body = QStringLiteral("<audio controls src=\"%1\" title=\"Voice memo\" "
                                            "data-anykeep-duration-ms=\"42000\"></audio>\n"
                                            "<div data-anykeep-audio-transcript=\"1\">Recorded transcript</div>")
                                 .arg(imported.value.uri());
        {
            PTFStorage writer(mediaWriter);
            QVERIFY(writer.setStoragePath(notesDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Audio"));
            note.setText(body, Note::Markdown);
            note.setMedia({ imported.value });
            QVERIFY(writer.saveNote(note));
        }

        LocalMediaStore mediaReader(mediaDirectory.path(), masterKey);
        PTFStorage      reader(mediaReader);
        QVERIFY(reader.setStoragePath(notesDirectory.path()));
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.media().size(), 1);
        QCOMPARE(loaded.media().constFirst().id, imported.value.id);
        QCOMPARE(loaded.media().constFirst().mediaType, QStringLiteral("audio/mp4"));
        const auto opened = mediaReader.data(loaded.media().constFirst().blobId);
        QVERIFY2(opened, qPrintable(opened.error));
        QCOMPARE(opened.value, audioBytes);
    }

    void genericAttachmentSurvivesFreshStorageInstance()
    {
        QTemporaryDir notesDirectory;
        QTemporaryDir mediaDirectory;
        QVERIFY(notesDirectory.isValid());
        QVERIFY(mediaDirectory.isValid());

        const auto       masterKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore  mediaWriter(mediaDirectory.path(), masterKey);
        const QByteArray fileBytes("portable-document-payload");
        const auto       imported
            = mediaWriter.importData(fileBytes, QStringLiteral("specification.pdf"), QStringLiteral("application/pdf"));
        QVERIFY2(imported, qPrintable(imported.error));

        const QString body = QStringLiteral("<a href=\"%1\" data-anykeep-attachment=\"1\" "
                                            "data-anykeep-media-type=\"application/pdf\" data-anykeep-size=\"%2\">"
                                            "specification.pdf</a>")
                                 .arg(imported.value.uri(), QString::number(fileBytes.size()));
        {
            PTFStorage writer(mediaWriter);
            QVERIFY(writer.setStoragePath(notesDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Attachment"));
            note.setText(body, Note::Markdown);
            note.setMedia({ imported.value });
            QVERIFY(writer.saveNote(note));
        }

        LocalMediaStore mediaReader(mediaDirectory.path(), masterKey);
        PTFStorage      reader(mediaReader);
        QVERIFY(reader.setStoragePath(notesDirectory.path()));
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.media().size(), 1);
        QCOMPARE(loaded.media().constFirst().id, imported.value.id);
        QCOMPARE(loaded.media().constFirst().mediaType, QStringLiteral("application/pdf"));
        const auto opened = mediaReader.data(loaded.media().constFirst().blobId);
        QVERIFY2(opened, qPrintable(opened.error));
        QCOMPARE(opened.value, fileBytes);
    }

    void noteSurvivesFreshStorageInstance()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSettings  settings;
        const auto previousPath    = settings.value(QStringLiteral("storage.ptf.path"));
        const bool hadPreviousPath = settings.contains(QStringLiteral("storage.ptf.path"));

        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(directory.path()));

            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Persistent note"));
            note.setText(QStringLiteral("Persistent body"), Note::Markdown);
            QVERIFY(writer.saveNote(note));
            QCOMPARE(writer.noteList().size(), 1);
        }

        {
            PTFStorage reader;
            QVERIFY(reader.setStoragePath(directory.path()));
            const auto notes = reader.noteList();
            QCOMPARE(notes.size(), 1);
            QCOMPARE(notes.first().title(), QStringLiteral("Persistent note"));

            Note loaded = notes.first();
            QVERIFY(loaded.load());
            QCOMPARE(loaded.text(), QStringLiteral("Persistent body"));
            QCOMPARE(loaded.format(), Note::Markdown);
        }

        if (hadPreviousPath)
            settings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
        else
            settings.remove(QStringLiteral("storage.ptf.path"));
    }

    void favoriteSurvivesFreshStorageInstanceAndLivesBesideFolderMetadata()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QString noteId;
        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(directory.path()));
            QVERIFY(writer.supportsFavorite());

            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Favorite PTF note"));
            note.setText(QStringLiteral("Body"), Note::Markdown);
            note.setFavorite(true);
            QVERIFY(writer.saveNote(note));

            const auto notes = writer.noteList();
            QCOMPARE(notes.size(), 1);
            QVERIFY(notes.constFirst().isFavorite());
            noteId = notes.constFirst().id();
        }

        QFile indexFile(directory.filePath(QStringLiteral(".anykeep-folders.json")));
        QVERIFY(indexFile.open(QIODevice::ReadOnly));
        const auto root = QJsonDocument::fromJson(indexFile.readAll()).object();
        QVERIFY(root.value(QStringLiteral("folders")).isArray());
        QVERIFY(root.value(QStringLiteral("assignments")).isArray());
        const auto favorites = root.value(QStringLiteral("favorites")).toArray();
        QCOMPARE(favorites.size(), 1);
        QCOMPARE(favorites.at(0).toString(), noteId);
        indexFile.close();

        QString renamedId;
        {
            PTFStorage reader;
            QVERIFY(reader.setStoragePath(directory.path()));
            const auto notes = reader.noteList();
            QCOMPARE(notes.size(), 1);
            QVERIFY(notes.constFirst().isFavorite());

            Note loaded = notes.constFirst();
            QVERIFY(loaded.load());
            QVERIFY(loaded.isFavorite());
            loaded.setTitle(QStringLiteral("Renamed favorite PTF note"));
            QVERIFY(reader.saveNote(loaded));

            const auto renamed = reader.noteList();
            QCOMPARE(renamed.size(), 1);
            renamedId = renamed.constFirst().id();
            QVERIFY(renamedId != noteId);
            QVERIFY(renamed.constFirst().isFavorite());
        }

        QVERIFY(indexFile.open(QIODevice::ReadOnly));
        const auto renamedRoot      = QJsonDocument::fromJson(indexFile.readAll()).object();
        const auto renamedFavorites = renamedRoot.value(QStringLiteral("favorites")).toArray();
        QCOMPARE(renamedFavorites.size(), 1);
        QCOMPARE(renamedFavorites.at(0).toString(), renamedId);
        indexFile.close();

        {
            PTFStorage reader;
            QVERIFY(reader.setStoragePath(directory.path()));
            Note loaded = reader.noteList().constFirst();
            QVERIFY(loaded.load());
            QVERIFY(loaded.isFavorite());
            loaded.setFavorite(false);
            QVERIFY(reader.saveNote(loaded));
        }

        PTFStorage reloaded;
        QVERIFY(reloaded.setStoragePath(directory.path()));
        const auto notes = reloaded.noteList();
        QCOMPARE(notes.size(), 1);
        QVERIFY(!notes.constFirst().isFavorite());

        QVERIFY(indexFile.open(QIODevice::ReadOnly));
        const auto updatedRoot = QJsonDocument::fromJson(indexFile.readAll()).object();
        QVERIFY(updatedRoot.value(QStringLiteral("favorites")).toArray().isEmpty());
        indexFile.close();

        Note favoriteAgain = notes.constFirst();
        QVERIFY(favoriteAgain.load());
        favoriteAgain.setFavorite(true);
        QVERIFY(reloaded.saveNote(favoriteAgain));
        reloaded.removeNote(favoriteAgain.id());

        QVERIFY(indexFile.open(QIODevice::ReadOnly));
        const auto removedRoot = QJsonDocument::fromJson(indexFile.readAll()).object();
        QVERIFY(removedRoot.value(QStringLiteral("favorites")).toArray().isEmpty());
        indexFile.close();
    }

    void legacyPtfMetadataIsMigrated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSettings  settings;
        const auto previousPath    = settings.value(QStringLiteral("storage.ptf.path"));
        const bool hadPreviousPath = settings.contains(QStringLiteral("storage.ptf.path"));
        struct SettingsGuard {
            QVariant previousPath;
            bool     hadPreviousPath;
            ~SettingsGuard()
            {
                QSettings settings;
                if (hadPreviousPath)
                    settings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
                else
                    settings.remove(QStringLiteral("storage.ptf.path"));
            }
        } guard { previousPath, hadPreviousPath };

        QFile note(directory.filePath(QStringLiteral("legacy.md")));
        QVERIFY(note.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(note.write("Legacy\n<a href=\"qtnote-media:/00000000-0000-0000-0000-000000000001/file.pdf\" "
                           "data-qtnote-attachment=\"1\">file.pdf</a>")
                > 0);
        note.close();
        QFile legacyIndex(directory.filePath(QStringLiteral(".qtnote-folders.json")));
        QVERIFY(legacyIndex.open(QIODevice::WriteOnly));
        QVERIFY(legacyIndex.write(R"JSON({"version":1,"folders":[],"assignments":[]})JSON") > 0);
        legacyIndex.close();

        settings.setValue(QStringLiteral("storage.ptf.path"), directory.path());
        PTFStorage storage;
        QVERIFY(storage.init());
        QVERIFY(storage.folderCatalogAvailable());
        QVERIFY(storage.folderCatalogErrorString().isEmpty());

        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral(".qtnote-folders.json"))));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral(".anykeep-folders.json"))));
        QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto contents = QString::fromUtf8(note.readAll());
        QVERIFY(contents.contains(QStringLiteral("anykeep-media:/")));
        QVERIFY(contents.contains(QStringLiteral("data-anykeep-attachment")));
    }

    void noteListDoesNotRetainPreInitDirectory()
    {
        QTemporaryDir storageDirectory;
        QTemporaryDir unrelatedDirectory;
        QVERIFY(storageDirectory.isValid());
        QVERIFY(unrelatedDirectory.isValid());

        QSettings settings;
        struct EnvironmentGuard {
            QVariant previousPath;
            bool     hadPreviousPath;
            QString  previousWorkingDirectory;

            ~EnvironmentGuard()
            {
                QSettings restoreSettings;
                if (hadPreviousPath)
                    restoreSettings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
                else
                    restoreSettings.remove(QStringLiteral("storage.ptf.path"));
                QDir::setCurrent(previousWorkingDirectory);
            }
        } guard { settings.value(QStringLiteral("storage.ptf.path")),
                  settings.contains(QStringLiteral("storage.ptf.path")), QDir::currentPath() };

        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(storageDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Initialized note"));
            note.setText(QStringLiteral("Persistent body"), Note::Markdown);
            QVERIFY(writer.saveNote(note));
        }

        QVERIFY(QDir::setCurrent(unrelatedDirectory.path()));
        PTFStorage reader;

        // A direct pre-init read uses QDir's default path, but FileStorage no
        // longer retains that result after init() selects the real directory.
        QCOMPARE(reader.noteList().size(), 0);

        QVERIFY(reader.init());
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().title(), QStringLiteral("Initialized note"));
    }

    void noteListReflectsExternalFileChanges()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QCOMPARE(storage.noteList().size(), 0);

        QFile external(directory.filePath(QStringLiteral("external.md")));
        QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(external.write("External title\nExternal body"), qint64(28));
        external.close();

        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().id(), QStringLiteral("external"));
        QCOMPARE(notes.first().title(), QStringLiteral("External title"));
    }

    void requestedModificationTimeSurvivesSave()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Ordered note"));
        note.setText(QStringLiteral("Body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        note = storage.noteList().first();
        QVERIFY(note.load());
        const auto requested = QDateTime::fromMSecsSinceEpoch(1'720'000'000'123, TimeZoneUTC);
        note.setLastChangeUTC(requested);
        note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), requested);
        QVERIFY(storage.saveNote(note));

        const auto saved = storage.noteList().first();
        QCOMPARE(saved.lastChangeUTC().toMSecsSinceEpoch(), requested.toMSecsSinceEpoch());
        QVERIFY(!saved.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).isValid());
    }

    void reorderChangesOnlyFileTimesAndPreservesBlockOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QVERIFY(storage.supportsNoteReordering());

        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Second"), baseTime.addSecs(-1)));
        QVERIFY(saveAt(QStringLiteral("Third"), baseTime.addSecs(-2)));

        const auto before = storage.noteList();
        QCOMPARE(before.size(), 3);
        QCOMPARE(before.at(0).title(), QStringLiteral("First"));
        QCOMPARE(before.at(1).title(), QStringLiteral("Second"));
        QCOMPARE(before.at(2).title(), QStringLiteral("Third"));

        QHash<QString, QByteArray> contents;
        for (const auto &note : before) {
            QFile file(note.backendValue(QStringLiteral("fileName")).toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            contents.insert(note.id(), file.readAll());
        }

        QSignalSpy invalidated(&storage, &NoteStorage::invalidated);
        auto      *job = storage.reorderNotesAsync({ before.at(0).id(), before.at(1).id() }, before.at(2).id());
        QVERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(invalidated.size(), 1);
        delete job;

        const auto after = storage.noteList();
        QCOMPARE(after.size(), 3);
        QCOMPARE(after.at(0).title(), QStringLiteral("Third"));
        QCOMPARE(after.at(1).title(), QStringLiteral("First"));
        QCOMPARE(after.at(2).title(), QStringLiteral("Second"));
        for (const auto &note : after) {
            QFile file(note.backendValue(QStringLiteral("fileName")).toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), contents.value(note.id()));
        }
    }

    void reorderUsesTheGapBeforeMovingNeighborTimestamps()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Second"), baseTime.addSecs(-4)));
        QVERIFY(saveAt(QStringLiteral("Third"), baseTime.addSecs(-8)));

        const auto before       = storage.noteList();
        const auto firstTimeMs  = before.at(0).lastChangeUTC().toMSecsSinceEpoch();
        const auto secondTimeMs = before.at(1).lastChangeUTC().toMSecsSinceEpoch();
        QSignalSpy invalidated(&storage, &NoteStorage::invalidated);

        auto *job = storage.reorderNoteAsync(before.at(2).id(), before.at(0).id());
        QVERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(invalidated.size(), 1);
        delete job;

        const auto after = storage.noteList();
        QCOMPARE(after.at(0).title(), QStringLiteral("First"));
        QCOMPARE(after.at(1).title(), QStringLiteral("Third"));
        QCOMPARE(after.at(2).title(), QStringLiteral("Second"));
        QCOMPARE(after.at(0).lastChangeUTC().toMSecsSinceEpoch(), firstTimeMs);
        QCOMPARE(after.at(2).lastChangeUTC().toMSecsSinceEpoch(), secondTimeMs);
        QVERIFY(after.at(1).lastChangeUTC() < after.at(0).lastChangeUTC());
        QVERIFY(after.at(1).lastChangeUTC() > after.at(2).lastChangeUTC());
    }

    void asyncSaveReturnsFinalIdForSubsequentReorder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Last"), baseTime.addSecs(-1)));
        const auto before = storage.noteList();
        QCOMPARE(before.size(), 2);

        Note transferred = storage.createNote();
        QVERIFY(transferred.id().isEmpty());
        transferred.setTitle(QStringLiteral("Transferred"));
        transferred.setText(QStringLiteral("Transferred body"), Note::Markdown);
        auto *saveJob = storage.saveNoteAsync(transferred);
        QTRY_VERIFY(saveJob->isFinished());
        QCOMPARE(saveJob->state(), StorageJob::Succeeded);
        const QString finalId = saveJob->result().id();
        QVERIFY(!finalId.isEmpty());
        const auto savedNotes = storage.noteList();
        QVERIFY(std::ranges::any_of(savedNotes, [&finalId](const Note &note) { return note.id() == finalId; }));
        delete saveJob;

        auto *reorderJob = storage.reorderNoteAsync(finalId, before.constLast().id());
        QVERIFY(reorderJob->isFinished());
        QCOMPARE(reorderJob->state(), StorageJob::Succeeded);
        delete reorderJob;

        const auto after = storage.noteList();
        QCOMPARE(after.size(), 3);
        QCOMPARE(after.constLast().id(), finalId);
        QCOMPARE(after.constLast().title(), QStringLiteral("Transferred"));
    }

    void folderMetadataRoundTripsWithoutRewritingTheNote()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QVERIFY(storage.supportsNativeFolders());
        QVERIFY(storage.supportsNativeFolderCatalog());

        const QUuid folderId = QUuid::createUuid();
        auto       *catalog  = storage.replaceNativeFolderCatalogAsync(catalogWithFolder(folderId));
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Folderable note"));
        note.setText(QStringLiteral("Body that must not be rewritten"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        const auto before = storage.noteList();
        QCOMPARE(before.size(), 1);
        Note       summary  = before.constFirst();
        const auto fileName = summary.backendValue(QStringLiteral("fileName")).toString();
        QFile      file(fileName);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto contents = file.readAll();
        file.close();
        const auto modified = QFileInfo(fileName).lastModified();

        summary.setFolderId(folderId);
        QSignalSpy changed(&storage, &NoteStorage::noteModified);
        auto      *move = storage.changeNoteFolderAsync(summary);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        QCOMPARE(move->result().folderId(), folderId);
        QCOMPARE(changed.size(), 1);
        delete move;

        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), contents);
        file.close();
        QCOMPARE(QFileInfo(fileName).lastModified(), modified);

        const auto moved = storage.noteList();
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.constFirst().folderId(), folderId);

        PTFStorage reader;
        QVERIFY(reader.setStoragePath(directory.path()));
        QVERIFY(reader.folderCatalogAvailable());
        const auto afterRestart = reader.noteList();
        QCOMPARE(afterRestart.size(), 1);
        QCOMPARE(afterRestart.constFirst().folderId(), folderId);
    }

    void folderAssignmentFollowsNoteRename()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const QUuid folderId = QUuid::createUuid();
        auto       *catalog  = storage.replaceNativeFolderCatalogAsync(catalogWithFolder(folderId));
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Before rename"));
        note.setText(QStringLiteral("Body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        note = storage.noteList().constFirst();
        note.setFolderId(folderId);
        auto *move = storage.changeNoteFolderAsync(note);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        delete move;

        const auto oldNoteId = note.id();
        QVERIFY(note.load());
        note.setTitle(QStringLiteral("After rename"));
        QVERIFY(storage.saveNote(note));

        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        const auto renamed = notes.constFirst();
        QVERIFY(renamed.id() != oldNoteId);
        QCOMPARE(renamed.folderId(), folderId);

        FolderCatalog catalogView;
        QVERIFY(!catalogView.replaceSnapshot(storage.nativeFolderCatalog()));
        const auto *oldAssignment = catalogView.assignment(PTFStorage::storageId, oldNoteId);
        QVERIFY(oldAssignment);
        QVERIFY(oldAssignment->tombstone);
        QCOMPARE(catalogView.folderForNote(PTFStorage::storageId, renamed.id()), folderId);

        PTFStorage reader;
        QVERIFY(reader.setStoragePath(directory.path()));
        const auto persisted = reader.noteList();
        QCOMPARE(persisted.size(), 1);
        QCOMPARE(persisted.constFirst().folderId(), folderId);
    }

    void corruptFolderIndexDoesNotBlockNotesAndCanRestoreBackup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const QUuid folderId = QUuid::createUuid();
        auto       *catalog  = storage.replaceNativeFolderCatalogAsync(catalogWithFolder(folderId));
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Recoverable folder index"));
        note.setText(QStringLiteral("Body remains readable"), Note::Markdown);
        QVERIFY(storage.saveNote(note));
        note = storage.noteList().constFirst();
        note.setFolderId(folderId);
        auto *move = storage.changeNoteFolderAsync(note);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        delete move;

        // Save the healthy primary once more so it is also retained as a
        // backup before simulating a damaged primary file.
        catalog = storage.replaceNativeFolderCatalogAsync(storage.nativeFolderCatalog());
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        const auto indexPath = directory.filePath(QStringLiteral(".anykeep-folders.json"));
        QVERIFY(QFile::exists(indexPath));
        QVERIFY(QFile::exists(indexPath + QStringLiteral(".bak")));
        QFile damaged(indexPath);
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(damaged.write("not a folder index") > 0);
        damaged.close();

        PTFStorage reader;
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 ".*PTF folder index is unavailable.*Invalid PTF folder index payload.*")));
        QVERIFY(reader.setStoragePath(directory.path()));
        QVERIFY(reader.isAccessible());
        QVERIFY(!reader.folderCatalogAvailable());
        QVERIFY(!reader.folderCatalogErrorString().isEmpty());
        const auto withoutFolders = reader.noteList();
        QCOMPARE(withoutFolders.size(), 1);
        QVERIFY(withoutFolders.constFirst().folderId().isNull());
        Note readable = withoutFolders.constFirst();
        QVERIFY(readable.load());
        QCOMPARE(readable.text(), QStringLiteral("Body remains readable"));

        QString    preservedPath;
        const auto restore = reader.restoreFolderCatalogBackup(&preservedPath);
        QVERIFY(!restore);
        QVERIFY(!preservedPath.isEmpty());
        QVERIFY(QFile::exists(preservedPath));
        QVERIFY(reader.folderCatalogAvailable());
        const auto restored = reader.noteList();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.constFirst().folderId(), folderId);
    }

    void corruptFolderIndexDoesNotTurnACommittedBodySaveIntoFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QUuid  firstFolder  = QUuid::createUuid();
        const QUuid  secondFolder = QUuid::createUuid();
        auto         snapshot     = catalogWithFolder(firstFolder);
        FolderRecord second;
        second.id         = secondFolder;
        second.name       = QStringLiteral("Later target");
        second.revision   = 1;
        second.modifiedAt = QDateTime::currentDateTimeUtc();
        snapshot.folders.append(second);

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        auto *catalog = storage.replaceNativeFolderCatalogAsync(snapshot);
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Save despite index failure"));
        note.setText(QStringLiteral("Original body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));
        note = storage.noteList().constFirst();
        note.setFolderId(firstFolder);
        auto *move = storage.changeNoteFolderAsync(note);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        delete move;

        // Retain the assignment in the backup, then damage only the primary.
        catalog = storage.replaceNativeFolderCatalogAsync(storage.nativeFolderCatalog());
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;
        QFile damaged(directory.filePath(QStringLiteral(".anykeep-folders.json")));
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(damaged.write("corrupt") > 0);
        damaged.close();

        QVERIFY(note.load());
        note.setFolderId(secondFolder);
        note.setText(QStringLiteral("Committed body"), Note::Markdown);
        QSignalSpy errors(&storage, &NoteStorage::storageErorr);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 ".*Failed to update PTF portable metadata.*Invalid PTF folder index payload.*")));
        QVERIFY(storage.saveNote(note));
        QCOMPARE(errors.size(), 1);
        QVERIFY(!storage.folderCatalogAvailable());

        Note saved = storage.note(note.id());
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.text(), QStringLiteral("Committed body"));
    }
};

QTEST_MAIN(PTFStorageTest)

#include "ptfstorage_test.moc"
