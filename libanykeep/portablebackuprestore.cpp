#include "portablebackuprestore.h"

#include "localmediastore.h"
#include "notedata.h"
#include "ptfstorage.h"
#include "secureenvelope.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

} // namespace AnyKeep
