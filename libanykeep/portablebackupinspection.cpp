#include "portablebackup.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <limits>

namespace AnyKeep {
namespace {
    constexpr qint64 MaximumManifestBytes = 32 * 1024 * 1024;
    const QString ManifestName = QStringLiteral("manifest.json");
    const QString FolderIndexName = QStringLiteral(".anykeep-folders.json");

    bool safeComponent(const QString &name)
    {
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
            || name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
            return false;
        for (const auto ch : name) {
            if (ch.unicode() < 32 || ch.unicode() == 127
                || QStringLiteral("<>:\"/\\|?*").contains(ch))
                return false;
        }
        return true;
    }

    bool safePath(const QString &path)
    {
        if (path.isEmpty() || QDir::isAbsolutePath(path))
            return false;
        for (const auto &component : path.split(QLatin1Char('/'))) {
            if (!safeComponent(component))
                return false;
        }
        return true;
    }

    // Reject aliases, not just byte-identical duplicate JSON paths. Portable
    // trees must not overwrite a different file on case-insensitive hosts.
    QString portablePathKey(const QString &path)
    {
        return path.normalized(QString::NormalizationForm_C).toCaseFolded();
    }

    bool decimal(const QJsonValue &value, qint64 *result)
    {
        if (!value.isString())
            return false;
        const auto s = value.toString();
        bool ok = false;
        const qint64 n = s.toLongLong(&ok);
        if (!ok || n < 0 || QString::number(n) != s)
            return false;
        *result = n;
        return true;
    }

    bool unsignedDecimal(const QJsonValue &value, quint64 *result)
    {
        if (!value.isString())
            return false;
        const auto s = value.toString();
        bool ok = false;
        const quint64 n = s.toULongLong(&ok);
        if (!ok || QString::number(n) != s)
            return false;
        *result = n;
        return true;
    }

    bool digest(const QJsonValue &value, QByteArray *result, bool optional = false)
    {
        if (!value.isString())
            return false;
        const auto hex = value.toString().toLatin1();
        if (optional && hex.isEmpty()) {
            result->clear();
            return true;
        }
        const auto bytes = QByteArray::fromHex(hex);
        if (bytes.size() != 32 || bytes.toHex() != hex)
            return false;
        *result = bytes;
        return true;
    }

    bool stringArray(const QJsonValue &value, QStringList *result)
    {
        if (!value.isArray())
            return false;
        for (const auto &entry : value.toArray()) {
            if (!entry.isString())
                return false;
            result->append(entry.toString());
        }
        return true;
    }

    bool strings(const QJsonObject &object, std::initializer_list<QString> keys)
    {
        for (const auto &key : keys) {
            if (!object.value(key).isString())
                return false;
        }
        return true;
    }

    bool timestamp(const QJsonValue &value, QDateTime *result, bool optional = true)
    {
        if (!value.isString())
            return false;
        const QString s = value.toString();
        if (optional && s.isEmpty()) {
            *result = {};
            return true;
        }
        *result = QDateTime::fromString(s, Qt::ISODateWithMs);
        return result->isValid() && result->toUTC().toString(Qt::ISODateWithMs) == s;
    }

    bool noteInfo(const QJsonObject &object, PortableBackupNoteInfo *note)
    {
        if (!strings(object, { QStringLiteral("ptfId"), QStringLiteral("sourceNoteId"),
                               QStringLiteral("originalFormat") }))
            return false;
        note->ptfId = object.value(QStringLiteral("ptfId")).toString();
        note->sourceNoteId = object.value(QStringLiteral("sourceNoteId")).toString();
        if (!safeComponent(note->ptfId))
            return false;
        if (object.contains(QStringLiteral("draftId"))) {
            if (!object.value(QStringLiteral("draftId")).isString())
                return false;
            note->draftId = QUuid(object.value(QStringLiteral("draftId")).toString());
            if (note->draftId.isNull())
                return false;
        } else if (note->sourceNoteId.isEmpty()) {
            return false;
        }
        const auto format = object.value(QStringLiteral("originalFormat")).toString();
        if (format == QStringLiteral("markdown"))
            note->originalFormat = Note::Markdown;
        else if (format == QStringLiteral("plain-text"))
            note->originalFormat = Note::PlainText;
        else if (format == QStringLiteral("html"))
            note->originalFormat = Note::Html;
        else
            return false;
        if (!timestamp(object.value(QStringLiteral("modifiedUtc")), &note->modifiedUtc)
            || !stringArray(object.value(QStringLiteral("tags")), &note->tags)
            || !object.value(QStringLiteral("unavailableMedia")).isArray())
            return false;
        for (const auto &entry : object.value(QStringLiteral("unavailableMedia")).toArray()) {
            const auto media = entry.toObject();
            PortableBackupMissingMedia missing;
            if (!strings(media, { QStringLiteral("id"), QStringLiteral("uri"), QStringLiteral("originalName"),
                                  QStringLiteral("mimeType") })
                || !decimal(media.value(QStringLiteral("size")), &missing.size)
                || !digest(media.value(QStringLiteral("sha256")), &missing.sha256, true))
                return false;
            missing.id = QUuid(media.value(QStringLiteral("id")).toString());
            missing.uri = media.value(QStringLiteral("uri")).toString();
            missing.originalName = media.value(QStringLiteral("originalName")).toString();
            missing.mimeType = media.value(QStringLiteral("mimeType")).toString();
            if (missing.id.isNull() || !missing.uri.startsWith(QStringLiteral("anykeep-media:/")))
                return false;
            note->unavailableMedia.append(std::move(missing));
        }
        return true;
    }

    bool draftInfo(const QJsonObject &object, PortableBackupDraftInfo *draft)
    {
        if (!strings(object, { QStringLiteral("id"), QStringLiteral("sourceStorageId"),
                               QStringLiteral("sourceNoteId"), QStringLiteral("operation"),
                               QStringLiteral("state"), QStringLiteral("transferSourceStorageId"),
                               QStringLiteral("transferSourceNoteId") }))
            return false;
        draft->id = QUuid(object.value(QStringLiteral("id")).toString());
        draft->storageId = object.value(QStringLiteral("sourceStorageId")).toString();
        draft->noteId = object.value(QStringLiteral("sourceNoteId")).toString();
        draft->transferSourceStorageId = object.value(QStringLiteral("transferSourceStorageId")).toString();
        draft->transferSourceNoteId = object.value(QStringLiteral("transferSourceNoteId")).toString();
        const auto operation = object.value(QStringLiteral("operation")).toString();
        if (operation == QStringLiteral("publish"))
            draft->operation = DraftRecord::Publish;
        else if (operation == QStringLiteral("delete"))
            draft->operation = DraftRecord::Delete;
        else
            return false;
        const auto state = object.value(QStringLiteral("state")).toString();
        static const QHash<QString, DraftRecord::State> states {
            { QStringLiteral("editing"), DraftRecord::Editing },
            { QStringLiteral("ready"), DraftRecord::Ready },
            { QStringLiteral("publishing"), DraftRecord::Publishing },
            { QStringLiteral("retry"), DraftRecord::Retry },
            { QStringLiteral("needs-routing"), DraftRecord::NeedsRouting },
            { QStringLiteral("deleting"), DraftRecord::Deleting }
        };
        const auto it = states.constFind(state);
        if (draft->id.isNull() || it == states.cend()
            || !unsignedDecimal(object.value(QStringLiteral("revision")), &draft->revision)
            || !timestamp(object.value(QStringLiteral("updatedUtc")), &draft->updatedAt))
            return false;
        draft->state = it.value();
        return true;
    }

    bool regularEntry(const QFileInfo &info)
    {
        if (info.isSymLink() || (!info.isDir() && !info.isFile()) || !info.isReadable())
            return false;
        // Also reject redirected directory entries (e.g. Windows junctions).
        const auto canonical = QDir::cleanPath(info.canonicalFilePath());
        const auto absolute = QDir::cleanPath(info.absoluteFilePath());
#ifdef Q_OS_WIN
        return canonical.compare(absolute, Qt::CaseInsensitive) == 0;
#else
        return canonical == absolute;
#endif
    }
} // namespace

PortableBackupInspection PortableBackup::inspectDirectory(const QString &directory, const Progress &progress,
                                                            const Continue &continueReading)
{
    const auto fail = [](const QString &message) {
        PortableBackupInspection invalid;
        invalid.error = message;
        return invalid;
    };
    const auto cancelled = [&] { return continueReading && !continueReading(); };
    if (cancelled())
        return fail(QStringLiteral("Backup inspection cancelled"));
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        return fail(QStringLiteral("An absolute backup directory is required"));
    const QFileInfo rootInfo(QDir::cleanPath(directory));
    if (rootInfo.isSymLink() || !rootInfo.isDir() || !rootInfo.isReadable())
        return fail(QStringLiteral("The backup directory is unavailable"));
    // Resolve the selected directory's ancestors once. All descendants must
    // then be ordinary entries in this tree, not links or redirected paths.
    const QDir root(rootInfo.canonicalFilePath());
    const QFileInfo manifestInfo(root.filePath(ManifestName));
    if (!manifestInfo.isFile() || !regularEntry(manifestInfo))
        return fail(QStringLiteral("The backup manifest is unavailable or unsafe"));
    QFile manifest(manifestInfo.absoluteFilePath());
    if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > MaximumManifestBytes)
        return fail(QStringLiteral("Could not read the backup manifest or its size exceeds the limit"));
    const QByteArray bytes = manifest.read(MaximumManifestBytes + 1);
    if (bytes.size() > MaximumManifestBytes || manifest.error() != QFileDevice::NoError || !manifest.atEnd())
        return fail(QStringLiteral("Could not read the complete backup manifest"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("Invalid backup manifest JSON"));
    const auto object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("anykeep-ptf-backup")
        || object.value(QStringLiteral("version")).toInt(-1) != 1)
        return fail(QStringLiteral("Unsupported backup format or version"));
    PortableBackupInspection result;
    if (!object.value(QStringLiteral("complete")).isBool()
        || !object.value(QStringLiteral("storages")).isArray()
        || !object.value(QStringLiteral("files")).isArray()
        || !timestamp(object.value(QStringLiteral("createdUtc")), &result.createdUtc, false))
        return fail(QStringLiteral("Invalid backup manifest fields or missing file inventory"));
    result.manifestSha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);

    QSet<QString> storageIds, allowedDirectories, requiredFiles, aliases;
    QSet<QString> draftIds;
    bool complete = true;
    int storageNumber = 0;
    for (const auto &entry : object.value(QStringLiteral("storages")).toArray()) {
        if (cancelled())
            return fail(QStringLiteral("Backup inspection cancelled"));
        const auto storageObject = entry.toObject();
        PortableBackupStorageInfo storage;
        storage.directory = storageObject.value(QStringLiteral("directory")).toString();
        storage.storageId = storageObject.value(QStringLiteral("storageId")).toString();
        storage.displayName = storageObject.value(QStringLiteral("displayName")).toString();
        if (storage.directory != QStringLiteral("storage-%1").arg(++storageNumber)
            || storage.storageId.isEmpty() || storageIds.contains(storage.storageId)
            || !storageObject.value(QStringLiteral("displayName")).isString()
            || !storageObject.value(QStringLiteral("remote")).isBool()
            || !storageObject.value(QStringLiteral("complete")).isBool()
            || !storageObject.value(QStringLiteral("notes")).isArray()
            || !storageObject.value(QStringLiteral("drafts")).isArray()
            || !stringArray(storageObject.value(QStringLiteral("omittedNoteIds")), &storage.omittedNoteIds))
            return fail(QStringLiteral("Invalid or duplicate source storage in backup"));
        storageIds.insert(storage.storageId);
        storage.remote = storageObject.value(QStringLiteral("remote")).toBool();
        storage.complete = storageObject.value(QStringLiteral("complete")).toBool();
        complete = complete && storage.complete && storage.omittedNoteIds.isEmpty();
        allowedDirectories.insert(storage.directory);
        const QString prefix = storage.directory + QLatin1Char('/');
        requiredFiles.insert(prefix + FolderIndexName);

        QSet<QString> identities, notePaths;
        QHash<QUuid, QString> draftNotes;
        for (const auto &noteEntry : storageObject.value(QStringLiteral("notes")).toArray()) {
            PortableBackupNoteInfo note;
            if (!noteInfo(noteEntry.toObject(), &note))
                return fail(QStringLiteral("Invalid portable note metadata"));
            const auto identity = note.draftId.isNull()
                ? QStringLiteral("note:") + note.sourceNoteId
                : QStringLiteral("draft:") + note.draftId.toString(QUuid::WithoutBraces);
            const auto notePath = prefix + note.ptfId;
            if (identities.contains(identity) || notePaths.contains(portablePathKey(notePath))
                || result.notesWritten == std::numeric_limits<int>::max()
                || note.unavailableMedia.size() > std::numeric_limits<int>::max() - result.unavailableMedia)
                return fail(QStringLiteral("Duplicate note identity or path in backup"));
            identities.insert(identity);
            notePaths.insert(portablePathKey(notePath));
            if (!note.draftId.isNull())
                draftNotes.insert(note.draftId, note.sourceNoteId);
            allowedDirectories.insert(notePath);
            requiredFiles.insert(notePath + QStringLiteral(".md"));
            ++result.notesWritten;
            result.unavailableMedia += int(note.unavailableMedia.size());
            storage.notes.append(std::move(note));
        }
        for (const auto &draftEntry : storageObject.value(QStringLiteral("drafts")).toArray()) {
            PortableBackupDraftInfo draft;
            if (!draftInfo(draftEntry.toObject(), &draft))
                return fail(QStringLiteral("Invalid draft intent in backup"));
            const auto id = draft.id.toString(QUuid::WithoutBraces);
            if (draftIds.contains(id)
                || (draft.operation == DraftRecord::Publish
                    && (!draftNotes.contains(draft.id) || draftNotes.value(draft.id) != draft.noteId))
                || (draft.operation == DraftRecord::Delete && draftNotes.contains(draft.id)))
                return fail(QStringLiteral("Inconsistent draft intent in backup"));
            draftIds.insert(id);
            storage.drafts.append(std::move(draft));
        }
        result.storages.append(std::move(storage));
    }

    complete = complete && result.unavailableMedia == 0;
    if (object.value(QStringLiteral("notesWritten")).toInt(-1) != result.notesWritten
        || object.value(QStringLiteral("unavailableMedia")).toInt(-1) != result.unavailableMedia
        || object.value(QStringLiteral("complete")).toBool() != complete)
        return fail(QStringLiteral("Inconsistent backup coverage or note counts"));
    result.complete = complete;

    QHash<QString, qsizetype> inventory;
    for (const auto &entry : object.value(QStringLiteral("files")).toArray()) {
        const auto fileObject = entry.toObject();
        PortableBackupFileInfo file;
        file.path = fileObject.value(QStringLiteral("path")).toString();
        if (!safePath(file.path) || file.path == ManifestName
            || aliases.contains(portablePathKey(file.path))
            || !decimal(fileObject.value(QStringLiteral("size")), &file.size)
            || !digest(fileObject.value(QStringLiteral("sha256")), &file.sha256))
            return fail(QStringLiteral("Unsafe or duplicate path in backup inventory"));
        const auto parts = file.path.split(QLatin1Char('/'));
        const bool allowed = requiredFiles.contains(file.path)
            || (parts.size() == 2 && allowedDirectories.contains(parts[0])
                && parts[1] == FolderIndexName + QStringLiteral(".bak"))
            || (parts.size() == 3 && allowedDirectories.contains(parts[0] + QLatin1Char('/') + parts[1]));
        if (!allowed)
            return fail(QStringLiteral("Unexpected file in backup inventory: %1").arg(file.path));
        aliases.insert(portablePathKey(file.path));
        inventory.insert(file.path, result.files.size());
        result.files.append(std::move(file));
    }
    for (const auto &path : requiredFiles) {
        if (!inventory.contains(path))
            return fail(QStringLiteral("Missing required backup file: %1").arg(path));
    }

    QSet<QString> seen;
    QSet<QString> treeAliases { portablePathKey(ManifestName) };
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    QDirIterator entries(root.absolutePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
    while (entries.hasNext()) {
        if (cancelled() || (progress && !progress(int(seen.size()), int(result.files.size()))))
            return fail(QStringLiteral("Backup inspection cancelled"));
        entries.next();
        const auto info = entries.fileInfo();
        const auto path = root.relativeFilePath(info.absoluteFilePath());
        if (!regularEntry(info) || !safePath(path))
            return fail(QStringLiteral("Unsafe backup filesystem entry: %1").arg(path));
        if (path == ManifestName)
            continue;
        const auto alias = portablePathKey(path);
        if (treeAliases.contains(alias))
            return fail(QStringLiteral("Aliased backup filesystem entry: %1").arg(path));
        treeAliases.insert(alias);
        if (info.isDir()) {
            if (!allowedDirectories.contains(path))
                return fail(QStringLiteral("Unexpected backup directory: %1").arg(path));
            continue;
        }
        const auto expected = inventory.constFind(path);
        if (expected == inventory.cend() || seen.contains(path))
            return fail(QStringLiteral("Unexpected backup file: %1").arg(path));
        const auto &fileInfo = result.files.at(expected.value());
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly) || file.size() != fileInfo.size)
            return fail(QStringLiteral("Backup file size mismatch: %1").arg(path));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 read = 0;
        while (read < fileInfo.size) {
            if (cancelled())
                return fail(QStringLiteral("Backup inspection cancelled"));
            const auto count = file.read(buffer.data(), qMin<qint64>(buffer.size(), fileInfo.size - read));
            if (count <= 0)
                return fail(QStringLiteral("Could not read backup file: %1").arg(path));
            hash.addData(QByteArrayView(buffer.constData(), count));
            read += count;
        }
        if (!file.atEnd() || file.size() != fileInfo.size || hash.result() != fileInfo.sha256)
            return fail(QStringLiteral("Backup file integrity mismatch: %1").arg(path));
        seen.insert(path);
    }
    if (seen.size() != inventory.size())
        return fail(QStringLiteral("One or more inventoried backup files are missing"));
    if (cancelled() || (progress && !progress(int(seen.size()), int(result.files.size()))))
        return fail(QStringLiteral("Backup inspection cancelled"));
    return result;
}

} // namespace AnyKeep
