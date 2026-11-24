#include "portablebackuprestore.h"

#include "localmediastore.h"
#include "notedata.h"
#include "ptfstorage.h"
#include "secureenvelope.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

namespace AnyKeep {
namespace {
    constexpr qint64 MaxDocumentBytes = 32 * 1024 * 1024;

    bool copyVerifiedFile(const QDir &source, const QDir &destination, const PortableBackupFileInfo &entry,
                          const PortableBackup::Continue &keepGoing)
    {
        const QFileInfo info(source.filePath(entry.path));
        if (!info.isFile() || info.isSymLink() || info.size() != entry.size)
            return false;
        const auto canonical = info.canonicalFilePath();
#ifdef Q_OS_WIN
        if (canonical.compare(info.absoluteFilePath(), Qt::CaseInsensitive) != 0)
#else
        if (canonical != info.absoluteFilePath())
#endif
            return false;
        QFile input(info.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly))
            return false;
        const auto target = destination.filePath(entry.path);
        if (!destination.mkpath(QFileInfo(target).absolutePath()))
            return false;
        QSaveFile output(target);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly))
            return false;
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        qint64 offset = 0;
        while (offset < entry.size) {
            if (keepGoing && !keepGoing())
                return false;
            const auto count = input.read(buffer.data(), qMin<qint64>(buffer.size(), entry.size - offset));
            if (count <= 0 || output.write(buffer.constData(), count) != count)
                return false;
            hash.addData(QByteArrayView(buffer.constData(), count));
            offset += count;
        }
        return input.atEnd() && input.size() == entry.size && hash.result() == entry.sha256 && output.commit();
    }

    bool missingReference(const PortableBackupMissingMedia &missing, MediaReference *reference)
    {
        const auto prefix = QStringLiteral("anykeep-media:/%1/").arg(missing.id.toString(QUuid::WithoutBraces));
        if (!missing.uri.startsWith(prefix))
            return false;
        const auto name = QUrl::fromPercentEncoding(missing.uri.mid(prefix.size()).toUtf8());
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
            || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
            return false;
        reference->id = missing.id;
        reference->portableName = name;
        reference->originalName = missing.originalName;
        reference->mediaType = missing.mimeType;
        reference->size = missing.size;
        reference->checksum = missing.sha256;
        return reference->uri() == missing.uri;
    }

    Note detached(const Note &source, const PortableBackupNoteInfo &info)
    {
        Note note(new NoteData(nullptr));
        note.setId(info.sourceNoteId);
        note.setTitle(source.title());
        note.setText(source.text(), source.format());
        note.setTags(info.tags);
        note.setFolderId(source.folderId());
        note.setFavorite(source.isFavorite());
        note.setLastChangeUTC(info.modifiedUtc);
        note.setMedia(source.media());
        return note;
    }
} // namespace

PortableBackupRestore::PortableBackupRestore() = default;
PortableBackupRestore::~PortableBackupRestore() = default;

std::unique_ptr<PortableBackupRestore> PortableBackupRestore::prepareDirectory(
    const QString &directory, const QByteArray &previewManifestSha256, const QString &privateParent,
    QString *error, const PortableBackup::Progress &progress, const PortableBackup::Continue &continueReading)
{
    const auto fail = [error](const QString &message) -> std::unique_ptr<PortableBackupRestore> {
        if (error)
            *error = message;
        return {};
    };
    const auto cancelled = [&] { return continueReading && !continueReading(); };
    if (error)
        error->clear();
    if (previewManifestSha256.size() != 32)
        return fail(QStringLiteral("A verified backup preview is required"));
    if (!QDir::isAbsolutePath(privateParent) || !QFileInfo(privateParent).isDir())
        return fail(QStringLiteral("An existing private staging directory is required"));
    const auto checked = PortableBackup::inspectDirectory(directory, {}, continueReading);
    if (!checked)
        return fail(checked.error);
    if (checked.manifestSha256 != previewManifestSha256)
        return fail(QStringLiteral("The backup has changed since its preview"));

    auto prepared = std::unique_ptr<PortableBackupRestore>(new PortableBackupRestore);
    prepared->workingDirectory_ = std::make_unique<QTemporaryDir>(
        QDir(privateParent).filePath(QStringLiteral("anykeep-restore-XXXXXX")));
    if (!prepared->workingDirectory_->isValid())
        return fail(prepared->workingDirectory_->errorString());
    const QDir work(prepared->workingDirectory_->path());
    if (!work.mkpath(QStringLiteral("ptf")))
        return fail(QStringLiteral("Could not create the private backup copy"));
    const QDir stage(work.filePath(QStringLiteral("ptf")));
    const QDir source(QFileInfo(directory).canonicalFilePath());

    // Copy only verified inventory entries and check their bytes again while
    // copying. Never open PTF against a user-writable source directory.
    for (const auto &entry : checked.files) {
        if (cancelled())
            return fail(QStringLiteral("Backup preparation cancelled"));
        const auto parts = entry.path.split(QLatin1Char('/'));
        if ((parts.size() == 2 || parts.last() == QStringLiteral(".anykeep-media.json"))
            && entry.size > MaxDocumentBytes)
            return fail(QStringLiteral("Portable document or index exceeds the supported size"));
        if (!copyVerifiedFile(source, stage, entry, continueReading))
            return fail(cancelled() ? QStringLiteral("Backup preparation cancelled")
                                    : QStringLiteral("Could not stage verified file: %1").arg(entry.path));
    }
    const QFileInfo manifest(source.filePath(QStringLiteral("manifest.json")));
    if (manifest.size() < 0 || manifest.size() > MaxDocumentBytes
        || !copyVerifiedFile(source, stage, { QStringLiteral("manifest.json"), manifest.size(), previewManifestSha256 },
                             continueReading))
        return fail(QStringLiteral("Could not stage the verified backup manifest"));

    prepared->inspection_ = PortableBackup::inspectDirectory(stage.absolutePath(), {}, continueReading);
    if (!prepared->inspection_)
        return fail(prepared->inspection_.error);
    if (prepared->inspection_.manifestSha256 != previewManifestSha256)
        return fail(QStringLiteral("The staged backup does not match its preview"));

    const auto key = SecureEnvelope::generateMasterKey();
    if (key.size() != 32)
        return fail(QStringLiteral("Could not create an isolated media key"));
    prepared->mediaStore_ = std::make_unique<LocalMediaStore>(work.filePath(QStringLiteral("media")), key);
    FolderCatalog catalog;
    int done = 0;
    const int total = prepared->inspection_.notesWritten;
    for (const auto &sourceInfo : prepared->inspection_.storages) {
        if (cancelled())
            return fail(QStringLiteral("Backup preparation cancelled"));
        PTFStorage ptf(*prepared->mediaStore_, stage.filePath(sourceInfo.directory));
        if (!ptf.init())
            return fail(QStringLiteral("Could not open the staged PTF folder index"));
        FolderCatalogSnapshot folders;
        folders.folders = ptf.nativeFolderCatalog().folders;
        if (const auto failure = catalog.merge(folders))
            return fail(failure.message);

        PortableBackupStorage storage;
        storage.storageId = sourceInfo.storageId;
        storage.displayName = sourceInfo.displayName;
        storage.remote = sourceInfo.remote;
        storage.complete = sourceInfo.complete;
        storage.drafts = sourceInfo.drafts;
        storage.omittedNoteIds = sourceInfo.omittedNoteIds;
        for (const auto &info : sourceInfo.notes) {
            if (cancelled() || (progress && !progress(done, total)))
                return fail(QStringLiteral("Backup preparation cancelled"));
            QString readError;
            const auto loaded = ptf.readSnapshot(info.ptfId, &readError);
            if (loaded.isNull() || !loaded.isLoaded())
                return fail(readError.isEmpty() ? QStringLiteral("Could not load a portable note") : readError);
            auto note = detached(loaded, info);
            auto media = note.media();
            QSet<QUuid> ids;
            for (const auto &item : media)
                ids.insert(item.id);
            for (const auto &missing : info.unavailableMedia) {
                MediaReference reference;
                if (ids.contains(missing.id) || !missingReference(missing, &reference))
                    return fail(QStringLiteral("Invalid or duplicate unavailable attachment identity"));
                ids.insert(reference.id);
                media.append(reference);
            }
            note.setMedia(media);
            storage.notes.append({ note, info.sourceNoteId, info.draftId });
            ++done;
        }
        prepared->storages_.append(std::move(storage));
    }
    if (cancelled() || (progress && !progress(done, total)))
        return fail(QStringLiteral("Backup preparation cancelled"));
    prepared->folders_ = catalog.snapshot();
    return prepared;
}


DraftStoreResult<QList<RecoveredNoteCopy>> PortableBackupRestore::prepareCopies(
    const QList<CopySelection> &selection, LocalMediaStore &destination,
    const PortableBackup::Progress &progress, const PortableBackup::Continue &continueReading) const
{
    const auto fail = [](DraftStoreError::Code code, const QString &message)
        -> DraftStoreResult<QList<RecoveredNoteCopy>> { return { {}, { code, message } }; };
    const auto cancelled = [&] { return continueReading && !continueReading(); };
    // Validate the entire selection before changing even the media cache.
    QSet<QPair<qsizetype, qsizetype>> selected;
    for (const auto &item : selection) {
        if (item.storageIndex < 0 || item.storageIndex >= storages_.size()
            || item.noteIndex < 0 || item.noteIndex >= storages_.at(item.storageIndex).notes.size())
            return fail(DraftStoreError::InvalidArgument, QStringLiteral("Invalid recovery selection"));
        const auto key = qMakePair(item.storageIndex, item.noteIndex);
        if (selected.contains(key))
            return fail(DraftStoreError::InvalidArgument, QStringLiteral("Duplicate recovery selection"));
        selected.insert(key);
    }

    // The verified PTF tree already contains the clear bytes. Reuse those
    // files instead of decrypting the temporary cache into another copy.
    const QDir stage(QDir(workingDirectory_->path()).filePath(QStringLiteral("ptf")));
    QHash<QString, QList<const PortableBackupFileInfo *>> sidecars;
    for (const auto &file : inspection_.files) {
        const auto parts = file.path.split(QLatin1Char('/'));
        if (parts.size() == 3 && parts.last() != QStringLiteral(".anykeep-media.json"))
            sidecars[parts[0] + QLatin1Char('/') + parts[1]].append(&file);
    }

    QList<RecoveredNoteCopy> copies;
    copies.reserve(selection.size());
    int done = 0;
    for (const auto &item : selection) {
        if (cancelled() || (progress && !progress(done, int(selection.size()))))
            return fail(DraftStoreError::Io, QStringLiteral("Recovery copy preparation cancelled"));
        const auto &storageInfo = inspection_.storages.at(item.storageIndex);
        const auto &noteInfo = storageInfo.notes.at(item.noteIndex);
        const auto &source = storages_.at(item.storageIndex).notes.at(item.noteIndex).snapshot;
        Note note = source;
        note.setId({});
        const auto candidates = sidecars.value(storageInfo.directory + QLatin1Char('/') + noteInfo.ptfId);
        QSet<QUuid> unavailable;
        for (const auto &missing : noteInfo.unavailableMedia)
            unavailable.insert(missing.id);

        QList<MediaReference> media;
        auto title = note.title();
        auto body = note.text();
        for (const auto &reference : source.media()) {
            if (cancelled())
                return fail(DraftStoreError::Io, QStringLiteral("Recovery copy preparation cancelled"));
            MediaReference copy = reference;
            copy.id = QUuid::createUuid();
            copy.blobId.clear();
            copy.remoteData.clear();
            copy.posterBlobId.clear();
            if (!unavailable.contains(reference.id)) {
                const PortableBackupFileInfo *matching = nullptr;
                for (const auto *candidate : candidates) {
                    if (candidate->size == reference.size && candidate->sha256 == reference.checksum) {
                        matching = candidate;
                        break;
                    }
                }
                if (!matching)
                    return fail(DraftStoreError::Corrupt, QStringLiteral("Missing verified recovery attachment"));
                const auto imported = destination.importFile(stage.filePath(matching->path), copy.id);
                if (!imported)
                    return fail(DraftStoreError::Io, imported.error);
                if (imported.value.size != reference.size || imported.value.checksum != reference.checksum)
                    return fail(DraftStoreError::Corrupt, QStringLiteral("Recovery attachment changed during import"));
                copy.blobId = imported.value.blobId;
            }
            title.replace(reference.uri(), copy.uri());
            body.replace(reference.uri(), copy.uri());
            media.append(std::move(copy));
        }
        note.setTitle(title);
        note.setText(body, note.format());
        note.setMedia(media);
        copies.append({ note, item.destinationStorageId });
        ++done;
    }
    if (cancelled() || (progress && !progress(done, int(selection.size()))))
        return fail(DraftStoreError::Io, QStringLiteral("Recovery copy preparation cancelled"));
    return { copies, {} };
}

} // namespace AnyKeep
