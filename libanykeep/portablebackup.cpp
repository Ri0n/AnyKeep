#include "portablebackup.h"

#include "ptfstorage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <limits>

namespace AnyKeep {
namespace {
    QJsonObject missingMediaEntry(const MediaReference &media)
    {
        return { { QStringLiteral("id"), media.id.toString(QUuid::WithoutBraces) },
                 { QStringLiteral("uri"), media.uri() },
                 { QStringLiteral("originalName"), media.originalName },
                 { QStringLiteral("mimeType"), media.mediaType },
                 { QStringLiteral("size"), QString::number(media.size) },
                 { QStringLiteral("sha256"), QString::fromLatin1(media.checksum.toHex()) } };
    }

    QString draftStateName(DraftRecord::State state)
    {
        switch (state) {
        case DraftRecord::Editing: return QStringLiteral("editing");
        case DraftRecord::Ready: return QStringLiteral("ready");
        case DraftRecord::Publishing: return QStringLiteral("publishing");
        case DraftRecord::Retry: return QStringLiteral("retry");
        case DraftRecord::NeedsRouting: return QStringLiteral("needs-routing");
        case DraftRecord::Deleting: return QStringLiteral("deleting");
        }
        return {};
    }

    QString formatName(Note::Format format)
    {
        switch (format) {
        case Note::Markdown:
            return QStringLiteral("markdown");
        case Note::PlainText:
            return QStringLiteral("plain-text");
        case Note::Html:
            return QStringLiteral("html");
        }
        return {};
    }
} // namespace

PortableBackupResult PortableBackup::exportDirectory(const QString &directory,
                                                       const QList<PortableBackupStorage> &storages,
                                                       const FolderCatalogSnapshot &folders,
                                                       LocalMediaStore &mediaStore, const Progress &progress,
                                                       const Continue &continueWriting, const Continue &beforePublish)
{
    PortableBackupResult result;
    const auto fail = [&result](const QString &message) {
        result.error = message;
        result.complete = false;
        return result;
    };

    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        return fail(QStringLiteral("An absolute backup directory is required"));
    const QFileInfo target(QDir::cleanPath(directory));
    if (target.exists() || target.isSymLink() || target.fileName().isEmpty() || !target.dir().exists())
        return fail(QStringLiteral("The backup destination must be a new directory in an existing parent"));

    const auto cancelled = [&] { return continueWriting && !continueWriting(); };
    if (cancelled())
        return fail(QStringLiteral("Backup cancelled"));

    int total = 0;
    QSet<QString> storageIds;
    for (const auto &storage : storages) {
        if (storage.storageId.isEmpty() || storageIds.contains(storage.storageId)
            || storage.notes.size() > std::numeric_limits<int>::max() - total)
            return fail(QStringLiteral("Invalid or duplicate source storage"));
        storageIds.insert(storage.storageId);
        total += int(storage.notes.size());
    }

    QTemporaryDir staging(target.dir().filePath(QStringLiteral(".anykeep-export-XXXXXX")));
    if (!staging.isValid())
        return fail(staging.errorString());

    // The temporary PTF stores share folder identities, but their assignments
    // must refer to their own saved note IDs rather than live provider IDs.
    FolderCatalogSnapshot portableFolders;
    portableFolders.folders = folders.folders;
    QJsonArray storageEntries;
    bool complete = true;
    int storageNumber = 0;

    for (const auto &storage : storages) {
        if (cancelled() || (progress && !progress(result.notesWritten, total)))
            return fail(QStringLiteral("Backup cancelled"));
        const QString folder = QStringLiteral("storage-%1").arg(++storageNumber);
        PTFStorage destination(mediaStore, staging.filePath(folder));
        if (!destination.init())
            return fail(QStringLiteral("Could not initialize the temporary PTF storage"));
        auto *catalogJob = destination.replaceNativeFolderCatalogAsync(portableFolders);
        const bool catalogSaved = catalogJob->state() == StorageJob::Succeeded;
        const auto catalogError = catalogJob->error();
        delete catalogJob;
        if (!catalogSaved)
            return fail(catalogError.message.isEmpty() ? QStringLiteral("Could not save the folder catalog")
                                                       : catalogError.message);

        QJsonArray notes;
        QSet<QString> exportedIdentities;
        for (const auto &entry : storage.notes) {
            if (cancelled() || (progress && !progress(result.notesWritten, total)))
                return fail(QStringLiteral("Backup cancelled"));
            const auto &source = entry.snapshot;
            if (source.isNull() || !source.isLoaded()
                || (entry.sourceNoteId.isEmpty() && entry.draftId.isNull()))
                return fail(QStringLiteral("The backup contains an unidentified or unloaded note snapshot"));
            const QString identity = entry.draftId.isNull()
                ? QStringLiteral("note:") + entry.sourceNoteId
                : QStringLiteral("draft:") + entry.draftId.toString(QUuid::WithoutBraces);
            if (exportedIdentities.contains(identity))
                return fail(QStringLiteral("The backup contains a duplicate note snapshot"));
            exportedIdentities.insert(identity);

            Note saved;
            QList<MediaReference> missing;
            if (!destination.copySnapshot(source, &saved, &missing, continueWriting) || saved.id().isEmpty())
                return fail(cancelled() ? QStringLiteral("Backup cancelled") : QStringLiteral("Could not export a PTF note"));
            QJsonArray unavailable;
            for (const auto &media : missing)
                unavailable.append(missingMediaEntry(media));
            result.unavailableMedia += int(missing.size());

            QJsonArray tags;
            for (const auto &tag : source.tags())
                tags.append(tag);
            QJsonObject noteEntry {
                { QStringLiteral("ptfId"), saved.id() },
                { QStringLiteral("sourceNoteId"), entry.sourceNoteId },
                { QStringLiteral("originalFormat"), formatName(source.format()) },
                { QStringLiteral("modifiedUtc"), source.lastChangeUTC().toUTC().toString(Qt::ISODateWithMs) },
                { QStringLiteral("tags"), tags },
                { QStringLiteral("unavailableMedia"), unavailable }
            };
            if (!entry.draftId.isNull())
                noteEntry.insert(QStringLiteral("draftId"), entry.draftId.toString(QUuid::WithoutBraces));
            // Do not serialize opaque provider backendData or remoteData: they
            // can contain credentials, local paths and nonportable locators.
            notes.append(noteEntry);
            ++result.notesWritten;
        }

        QJsonArray drafts;
        for (const auto &draft : storage.drafts) {
            drafts.append(QJsonObject {
                { QStringLiteral("id"), draft.id.toString(QUuid::WithoutBraces) },
                { QStringLiteral("sourceStorageId"), draft.storageId },
                { QStringLiteral("sourceNoteId"), draft.noteId },
                { QStringLiteral("operation"), draft.operation == DraftRecord::Delete
                      ? QStringLiteral("delete") : QStringLiteral("publish") },
                { QStringLiteral("state"), draftStateName(draft.state) },
                { QStringLiteral("transferSourceStorageId"), draft.transferSourceStorageId },
                { QStringLiteral("transferSourceNoteId"), draft.transferSourceNoteId },
                { QStringLiteral("revision"), QString::number(draft.revision) },
                { QStringLiteral("updatedUtc"), draft.updatedAt.toUTC().toString(Qt::ISODateWithMs) }
            });
        }
        storageEntries.append(QJsonObject {
            { QStringLiteral("directory"), folder },
            { QStringLiteral("storageId"), storage.storageId },
            { QStringLiteral("displayName"), storage.displayName },
            { QStringLiteral("remote"), storage.remote },
            { QStringLiteral("complete"), storage.complete },
            { QStringLiteral("notes"), notes },
            { QStringLiteral("drafts"), drafts },
            { QStringLiteral("omittedNoteIds"), QJsonArray::fromStringList(storage.omittedNoteIds) }
        });
        complete = complete && storage.complete && storage.omittedNoteIds.isEmpty();
    }

    if (cancelled() || (progress && !progress(result.notesWritten, total)))
        return fail(QStringLiteral("Backup cancelled"));

    // Inventory the actual staged files, including the PTF sidecar indexes.
    // Stream the readback so large media files do not require another full
    // in-memory copy. The manifest itself is not part of its own inventory.
    QJsonArray inventory;
    QStringList relativePaths;
    QDir root(staging.path());
    QDirIterator files(staging.path(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        if (cancelled())
            return fail(QStringLiteral("Backup cancelled"));
        files.next();
        const auto info = files.fileInfo();
        if (info.isSymLink() || (!info.isDir() && !info.isFile()))
            return fail(QStringLiteral("Unexpected entry in the staged backup"));
        if (info.isFile())
            relativePaths.append(root.relativeFilePath(info.absoluteFilePath()));
    }
    relativePaths.sort(Qt::CaseSensitive);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (const auto &path : relativePaths) {
        QFile file(root.filePath(path));
        if (!file.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Could not read back a staged backup file"));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const qint64 expectedSize = file.size();
        qint64 size = 0;
        while (true) {
            if (cancelled())
                return fail(QStringLiteral("Backup cancelled"));
            const qint64 count = file.read(buffer.data(), buffer.size());
            if (count < 0)
                return fail(QStringLiteral("Could not read back a staged backup file"));
            if (count == 0)
                break;
            hash.addData(QByteArrayView(buffer.constData(), count));
            size += count;
        }
        if (size != expectedSize || file.size() != expectedSize)
            return fail(QStringLiteral("A staged backup file changed during verification"));
        inventory.append(QJsonObject {
            { QStringLiteral("path"), path },
            { QStringLiteral("size"), QString::number(size) },
            { QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()) }
        });
    }

    complete = complete && result.unavailableMedia == 0;
    const QJsonObject manifest {
        { QStringLiteral("format"), QStringLiteral("anykeep-ptf-backup") },
        { QStringLiteral("version"), 1 },
        { QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { QStringLiteral("complete"), complete },
        { QStringLiteral("notesWritten"), result.notesWritten },
        { QStringLiteral("unavailableMedia"), result.unavailableMedia },
        { QStringLiteral("storages"), storageEntries },
        { QStringLiteral("files"), inventory }
    };
    const auto bytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (bytes.size() > MaximumManifestBytes)
        return fail(QStringLiteral("The backup manifest exceeds the supported size limit"));
    QSaveFile manifestFile(staging.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(bytes) != bytes.size()
        || !manifestFile.commit())
        return fail(QStringLiteral("Could not commit the backup manifest"));

    if (cancelled() || (beforePublish && !beforePublish()))
        return fail(QStringLiteral("Backup cancelled"));
    const QFileInfo finalTarget(target.absoluteFilePath());
    if (finalTarget.exists() || finalTarget.isSymLink()
        || !target.dir().rename(staging.path(), target.absoluteFilePath()))
        return fail(QStringLiteral("Could not publish the staged backup directory"));
    staging.setAutoRemove(false);
    result.complete = complete;
    return result;
}

} // namespace AnyKeep
