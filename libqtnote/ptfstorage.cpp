/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Contacts:
E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
*/

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_WIN
#include <QLibrary>
#include <shlobj.h>
#endif // Q_OS_WIN

#include "localmediastore.h"
#include "notedata.h"
#include "ptffolderindex.h"
#include "ptfstorage.h"
#include "utils.h"

namespace QtNote {

Q_LOGGING_CATEGORY(logPtfStorage, "qtnote.persistence.ptf")

namespace {
    const QString MediaManifestName = QStringLiteral(".qtnote-media.json");
    const QString FolderIndexName   = QStringLiteral(".qtnote-folders.json");

    struct SidecarMetadata {
        QUuid   id;
        QString originalName;
        QString mediaType;
    };

    QString diagnosticName(const QString &value)
    {
        if (value.isEmpty())
            return QStringLiteral("<empty>");
        const auto digest = QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex();
        return QString::fromLatin1(digest.left(12));
    }

    void logDirectorySnapshot(const QDir &dir, const char *phase)
    {
        const QFileInfo directoryInfo(dir.absolutePath());
        const auto      entries = dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        QStringList     descriptions;
        descriptions.reserve(entries.size());
        for (const auto &entry : entries) {
            descriptions.append(QStringLiteral("%1.%2(%3)")
                                    .arg(diagnosticName(entry.completeBaseName()), entry.suffix())
                                    .arg(entry.size()));
        }
        qCInfo(logPtfStorage).noquote() << phase << "path=" << dir.absolutePath() << "exists=" << directoryInfo.exists()
                                        << "directory=" << directoryInfo.isDir()
                                        << "readable=" << directoryInfo.isReadable()
                                        << "writable=" << directoryInfo.isWritable() << "files=" << entries.size()
                                        << descriptions.join(QStringLiteral(", "));
    }

    QHash<QString, SidecarMetadata> readMediaMetadata(const QDir &sidecar)
    {
        QHash<QString, SidecarMetadata> result;
        QFile                           file(sidecar.filePath(MediaManifestName));
        if (!file.open(QIODevice::ReadOnly))
            return result;
        const auto document = QJsonDocument::fromJson(file.readAll());
        for (const auto value : document.array()) {
            const auto  object = value.toObject();
            const QUuid id(object.value(QStringLiteral("id")).toString());
            const auto  name = object.value(QStringLiteral("file")).toString();
            if (!id.isNull() && !name.isEmpty()) {
                result.insert(name,
                              { id, object.value(QStringLiteral("originalName")).toString(),
                                object.value(QStringLiteral("mediaType")).toString() });
            }
        }
        return result;
    }

    QString uniqueName(QString proposed, QSet<QString> &used)
    {
        if (!used.contains(proposed)) {
            used.insert(proposed);
            return proposed;
        }
        const QFileInfo info(proposed);
        const auto      base   = info.completeBaseName();
        const auto      suffix = info.suffix();
        for (int i = 2;; ++i) {
            const auto candidate
                = base + QStringLiteral(" (%1)").arg(i) + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
            if (!used.contains(candidate)) {
                used.insert(candidate);
                return candidate;
            }
        }
    }

    QString importSidecarImages(QString body, const QString &noteId, const QDir &notesDir, QList<MediaReference> &media)
    {
        const QDir sidecar(notesDir.filePath(noteId));
        if (!sidecar.exists())
            return body;
        const auto                      metadata = readMediaMetadata(sidecar);
        static const QRegularExpression imagePattern(QStringLiteral(R"(!\[([^\]]*)\]\(([^\s\)]+)(?:\s+"[^"]*")?\))"));
        auto                            matches = imagePattern.globalMatch(body);
        struct Replacement {
            qsizetype start;
            qsizetype length;
            QString   value;
        };
        QList<Replacement> replacements;
        while (matches.hasNext()) {
            const auto match  = matches.next();
            const auto target = QUrl::fromPercentEncoding(match.captured(2).toUtf8());
            QString    mediaName;
            QUuid      uriId;
            if (target.startsWith(QStringLiteral("qtnote-media:/"))) {
                const auto parts = QUrl(target).path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
                if (parts.size() != 2)
                    continue;
                uriId                      = QUuid(parts.at(0));
                mediaName                  = parts.at(1);
                const auto alreadyImported = std::find_if(media.cbegin(), media.cend(),
                                                          [&uriId](const auto &item) { return item.id == uriId; });
                if (alreadyImported != media.cend())
                    continue;
            } else {
                const QFileInfo relative(target);
                if (relative.isAbsolute() || target.contains(QStringLiteral(".."))
                    || target.contains(QLatin1Char('\\')))
                    continue;
                mediaName = target;
                if (mediaName.startsWith(noteId + QLatin1Char('/')))
                    mediaName.remove(0, noteId.size() + 1);
                if (mediaName.contains(QLatin1Char('/')))
                    continue;
            }
            const auto item     = metadata.value(mediaName);
            auto       imported = LocalMediaStore::instance()->importFile(sidecar.filePath(mediaName),
                                                                    uriId.isNull() ? item.id : uriId);
            if (!imported)
                continue;
            if (!item.originalName.isEmpty())
                imported.value.originalName = item.originalName;
            if (!item.mediaType.isEmpty())
                imported.value.mediaType = item.mediaType;
            media.append(imported.value);
            replacements.prepend({ match.capturedStart(2), match.capturedLength(2), imported.value.uri() });
        }
        for (const auto &replacement : replacements)
            body.replace(replacement.start, replacement.length, replacement.value);
        return body;
    }
} // namespace

PTFStorage::PTFStorage(QObject *parent) : FileStorage(parent), icon(QLatin1String(":/icons/trayicon"))
{
    fileExt.append(QLatin1String("txt"));
    fileExt.append(QLatin1String("md"));
}

bool PTFStorage::init()
{
#ifdef Q_OS_ANDROID
    // The mobile storage is always private application data. A stale or
    // manually supplied desktop path must not redirect it to shared storage.
    notesDir.setPath(findStorageDir());
#else
    auto path = QSettings().value("storage.ptf.path").toString();
    notesDir.setPath(path.isEmpty() ? findStorageDir() : path);
    if (!notesDir.isReadable() && !path.isEmpty())
        notesDir.setPath(findStorageDir()); // try default
#endif
    qCInfo(logPtfStorage) << "Initializing PTF storage at" << notesDir.absolutePath();
    if (!notesDir.exists()) {
        const bool created = notesDir.mkpath(QLatin1String("."));
        qCInfo(logPtfStorage) << "Created PTF storage directory:" << created;
        if (!created)
            qCWarning(logPtfStorage) << "Could not create PTF storage directory" << notesDir.absolutePath();
    }
    logDirectorySnapshot(notesDir, "PTF init snapshot:");
    const bool accessible = isAccessible();
    if (accessible)
        loadFolderCatalog();
    qCInfo(logPtfStorage) << "PTF initialization result: accessible=" << accessible;
    return accessible;
}

bool PTFStorage::isAccessible() const
{
    const bool readable = notesDir.isReadable();
    return readable;
}

const QString PTFStorage::systemName() const { return storageId; }

const QString PTFStorage::name() const { return tr("Plain Text Storage"); }

QIcon PTFStorage::storageIcon() const { return icon; }

QIcon PTFStorage::noteIcon() const { return icon; }

QList<Note> PTFStorage::noteListFromInfoList(const QFileInfoList &files)
{
    qCInfo(logPtfStorage) << "Building PTF note list from" << files.size() << "filesystem entries";
    QList<Note> ret;
    foreach (const QFileInfo &fi, files) {
        // canonicalFilePath() can be empty for valid files exposed through
        // Android's app-specific external storage. The directory listing has
        // already resolved the entry, so its absolute path is the appropriate
        // stable backend path here.
        const QString filePath = fi.absoluteFilePath();
        QFile         file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(logPtfStorage) << "Failed to open PTF note while scanning: idHash="
                                     << diagnosticName(fi.completeBaseName()) << "suffix=" << fi.suffix()
                                     << file.errorString();
            continue;
        }

        Note note(new NoteData(this));
        note.setId(fi.completeBaseName());
        note.setFolderId(folderIdForNote(note.id()));
        note.setFormat(fi.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                          : Note::PlainText);
        note.setTitle(QString::fromUtf8(file.readLine()).trimmed());
        note.setTags(NoteData::tagsFromLine(QString::fromUtf8(file.readLine())));
        note.setLastChangeUTC(fi.lastModified());
        note.setBackendValue(QStringLiteral("fileName"), filePath);
        note.unload();
        ret.append(note);
        qCInfo(logPtfStorage) << "Discovered PTF note idHash=" << diagnosticName(note.id())
                              << "format=" << int(note.format()) << "size=" << fi.size()
                              << "modified=" << fi.lastModified();
    }
    qCInfo(logPtfStorage) << "PTF scan produced" << ret.size() << "notes";
    return ret;
}

Note PTFStorage::createNote()
{
    Note note(new NoteData(this));
    note.setFormat(Note::Markdown);
    note.setText(QString(), Note::Markdown);
    note.setLastChangeUTC(QDateTime::currentDateTimeUtc());
    return note;
}

Note PTFStorage::note(const QString &noteId)
{
    qCInfo(logPtfStorage) << "Looking up PTF note idHash=" << diagnosticName(noteId);
    if (!noteId.isEmpty()) {
        QFileInfo fi;
        for (auto const &ext : std::as_const(fileExt)) {
            fi = QFileInfo(QDir(notesDir).absoluteFilePath(QString("%1.%2").arg(noteId, ext)));
            if (fi.exists()) {
                Note loaded(new NoteData(this));
                loaded.setId(fi.completeBaseName());
                loaded.setFolderId(folderIdForNote(loaded.id()));
                loaded.setFormat(fi.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                                    : Note::PlainText);
                loaded.setLastChangeUTC(fi.lastModified());
                loaded.setBackendValue(QStringLiteral("fileName"), fi.filePath());
                if (loadNote(loaded)) {
                    qCInfo(logPtfStorage)
                        << "Loaded PTF note idHash=" << diagnosticName(noteId) << "suffix=" << fi.suffix();
                    return loaded;
                }
                qCWarning(logPtfStorage) << "Failed to load existing PTF note idHash=" << diagnosticName(noteId)
                                         << "suffix=" << fi.suffix();
                handleFSError();
                return {};
            }
        }
    }
    qCInfo(logPtfStorage) << "PTF note was not found, idHash=" << diagnosticName(noteId);
    return Note();
}

bool PTFStorage::loadNote(Note &note)
{
    QString fileName = note.backendValue(QStringLiteral("fileName")).toString();
    if (fileName.isEmpty()) {
        for (const auto &ext : std::as_const(fileExt)) {
            const auto candidate = notesDir.absoluteFilePath(QStringLiteral("%1.%2").arg(note.id(), ext));
            if (QFileInfo::exists(candidate)) {
                fileName = candidate;
                break;
            }
        }
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(logPtfStorage) << "Failed to open PTF note: idHash=" << diagnosticName(note.id())
                                 << "suffix=" << QFileInfo(fileName).suffix() << file.errorString();
        return false;
    }

    QString contents = QString::fromUtf8(file.readAll());
    qCInfo(logPtfStorage) << "Read PTF note idHash=" << diagnosticName(note.id())
                          << "suffix=" << QFileInfo(fileName).suffix() << "bytes=" << file.size();
    QList<MediaReference> media;
    if (QFileInfo(fileName).suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0)
        contents = importSidecarImages(contents, QFileInfo(fileName).completeBaseName(), notesDir, media);
    auto [title, body] = Utils::splitTitle(contents);
    note.setTitle(title);
    note.setText(body,
                 QFileInfo(fileName).suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                                     : Note::PlainText);
    note.setLastChangeUTC(QFileInfo(file).lastModified());
    note.setBackendValue(QStringLiteral("fileName"), fileName);
    note.setMedia(media);
    note.setFolderId(folderIdForNote(note.id()));
    return true;
}

bool PTFStorage::saveNote(const Note &note)
{
    qCInfo(logPtfStorage) << "Saving PTF note: idHash=" << diagnosticName(note.id()) << "format=" << int(note.format())
                          << "titleLength=" << note.title().size() << "bodyLength=" << note.text().size()
                          << "media=" << note.media().size() << "storagePath=" << notesDir.absolutePath();
    logDirectorySnapshot(notesDir, "PTF pre-save snapshot:");
    if (note.isEmpty()) {
        if (!note.id().isEmpty()) {
            removeNote(note.id()); // TODO check errors?
        }
        return true;
    }

    QString    oldNoteId         = note.id();
    const bool existedBeforeSave = noteFileExists(oldNoteId);
    QString    newNoteId         = oldNoteId;
    auto       ext               = QString(QLatin1String(note.format() == Note::Markdown ? "md" : "txt"));
    auto       fileName          = Utils::fileNameForText(notesDir, note.title(), ext, newNoteId);
    if (fileName.isEmpty()) {
        qCWarning(logPtfStorage) << "PTF filename generation failed: idHash=" << diagnosticName(oldNoteId)
                                 << "titleLength=" << note.title().size() << "extension=" << ext;
        return false;
    }
    qCInfo(logPtfStorage) << "PTF save target: oldIdHash=" << diagnosticName(oldNoteId)
                          << "newIdHash=" << diagnosticName(newNoteId) << "suffix=" << ext;
    QString       contents = note.title() + QLatin1Char('\n') + note.text();
    QDir          sidecar(notesDir.filePath(newNoteId));
    QSet<QString> materializedFiles;
    if (!note.media().isEmpty()) {
        if (!notesDir.mkpath(newNoteId)) {
            handleFSError();
            return false;
        }
        QJsonArray    manifest;
        QSet<QString> usedNames;
        for (const auto &reference : note.media()) {
            const auto loaded = LocalMediaStore::instance()->data(reference.blobId);
            if (!loaded) {
                qWarning() << "Failed to materialize note attachment:" << loaded.error;
                return false;
            }
            const auto materializedName
                = uniqueName(Utils::portableFileName(reference.portableName, QStringLiteral("attachment")), usedNames);
            materializedFiles.insert(materializedName);
            QSaveFile mediaFile(sidecar.filePath(materializedName));
            if (!mediaFile.open(QIODevice::WriteOnly) || mediaFile.write(loaded.value) != loaded.value.size()
                || !mediaFile.commit()) {
                handleFSError();
                return false;
            }
            contents.replace(reference.uri(),
                             QString::fromUtf8(QUrl::toPercentEncoding(newNoteId)) + QLatin1Char('/')
                                 + QString::fromUtf8(QUrl::toPercentEncoding(materializedName)));
            QJsonObject item;
            item.insert(QStringLiteral("id"), reference.id.toString(QUuid::WithoutBraces));
            item.insert(QStringLiteral("file"), materializedName);
            item.insert(QStringLiteral("originalName"), reference.originalName);
            item.insert(QStringLiteral("mediaType"), reference.mediaType);
            manifest.append(item);
        }
        QSaveFile  manifestFile(sidecar.filePath(MediaManifestName));
        const auto manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(manifestBytes) != manifestBytes.size()
            || !manifestFile.commit()) {
            handleFSError();
            return false;
        }
    }

    QSaveFile  file(fileName);
    const auto bytes = contents.trimmed().toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(logPtfStorage) << "Failed to open PTF save target: idHash=" << diagnosticName(newNoteId)
                                 << "suffix=" << ext << file.errorString();
        handleFSError();
        return false;
    }
    const qint64 written = file.write(bytes);
    if (written != bytes.size()) {
        qCWarning(logPtfStorage) << "Short PTF write: idHash=" << diagnosticName(newNoteId) << "suffix=" << ext
                                 << "expected=" << bytes.size() << "written=" << written << file.errorString();
        file.cancelWriting();
        handleFSError();
        return false;
    }
    if (!file.commit()) {
        qCWarning(logPtfStorage) << "Failed to commit PTF note: idHash=" << diagnosticName(newNoteId)
                                 << "suffix=" << ext << file.errorString();
        handleFSError();
        return false;
    }
    const auto requestedModified
        = note.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).toDateTime();
    if (requestedModified.isValid()) {
        QFile committedFile(fileName);
        if (!committedFile.open(QIODevice::ReadWrite)
            || !committedFile.setFileTime(requestedModified, QFileDevice::FileModificationTime)) {
            qCWarning(logPtfStorage) << "Failed to preserve requested PTF modification time: idHash="
                                     << diagnosticName(newNoteId) << committedFile.errorString();
            return false;
        }
        committedFile.close();
    }
    const QFileInfo committed(fileName);
    qCInfo(logPtfStorage) << "Committed PTF note: idHash=" << diagnosticName(newNoteId) << "suffix=" << ext
                          << "exists=" << committed.exists() << "size=" << committed.size()
                          << "readable=" << committed.isReadable() << "writable=" << committed.isWritable();
    if (note.media().isEmpty()) {
        sidecar.removeRecursively();
    } else {
        materializedFiles.insert(MediaManifestName);
        const auto staleFiles = sidecar.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const auto &staleFile : staleFiles) {
            if (!materializedFiles.contains(staleFile))
                sidecar.remove(staleFile);
        }
    }
    Note saved = note;
    saved.setId(newNoteId);
    saved.setLastChangeUTC(QFileInfo(fileName).lastModified());
    saved.removeBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey));
    saved.setBackendValue(QStringLiteral("fileName"), fileName);
    if (const auto folderError = updateFolderAssignment(oldNoteId, saved)) {
        qCWarning(logPtfStorage) << "Failed to update PTF folder index:" << folderError.message;
        emit storageErorr(
            tr("Failed to update the PTF folder index. The note content was saved, but its folder may need recovery."));
        // Folder organization is recoverable metadata. Do not report a body
        // save as failed after its QSaveFile commit succeeded: doing so would
        // make a damaged folder index block ordinary editing and can cause a
        // draft retry to create a duplicate after a title-based rename.
    }
    if (!oldNoteId.isEmpty()) {
        if (oldNoteId != newNoteId) {
            for (auto const &ext : std::as_const(fileExt)) {
                notesDir.remove(oldNoteId + QLatin1Char('.') + ext);
            }
            QDir(notesDir.filePath(oldNoteId)).removeRecursively();
        } else {
            for (auto const &otherExt : std::as_const(fileExt)) {
                if (ext != otherExt) {
                    notesDir.remove(oldNoteId + QLatin1Char('.') + otherExt);
                }
            }
        }
    }
    notifyNoteSaved(saved, oldNoteId, existedBeforeSave);
    logDirectorySnapshot(notesDir, "PTF post-save snapshot:");
    qCInfo(logPtfStorage) << "PTF save completed: oldIdHash=" << diagnosticName(oldNoteId)
                          << "newIdHash=" << diagnosticName(newNoteId);
    return true;
}

NoteReorderJob *PTFStorage::reorderNotesAsync(const QStringList &noteIds, const QString &afterNoteId, QObject *owner)
{
    auto *job = new NoteReorderJob(owner ? owner : this);
    job->start();
    StorageError error;
    const auto   changes = noteReorderChanges(noteIds, afterNoteId, &error);
    if (error) {
        job->fail(error);
        return job;
    }
    if (changes.isEmpty()) {
        job->complete();
        return job;
    }

    struct AppliedChange {
        QString   fileName;
        QDateTime previousModified;
    };
    QList<AppliedChange> applied;
    applied.reserve(changes.size());

    const auto rollBack = [&applied]() {
        for (auto it = applied.crbegin(); it != applied.crend(); ++it) {
            QFile file(it->fileName);
            if (file.open(QIODevice::ReadWrite)) {
                file.setFileTime(it->previousModified, QFileDevice::FileModificationTime);
                file.close();
            }
        }
    };

    for (const auto &change : changes) {
        QString fileName = change.note.backendValue(QStringLiteral("fileName")).toString();
        if (fileName.isEmpty()) {
            for (const auto &ext : std::as_const(fileExt)) {
                const auto candidate = notesDir.absoluteFilePath(change.note.id() + QLatin1Char('.') + ext);
                if (QFileInfo::exists(candidate)) {
                    fileName = candidate;
                    break;
                }
            }
        }

        QFile      file(fileName);
        const auto previousModified = QFileInfo(fileName).lastModified();
        if (fileName.isEmpty() || !previousModified.isValid() || !file.open(QIODevice::ReadWrite)
            || !file.setFileTime(change.modified, QFileDevice::FileModificationTime)) {
            const auto message = fileName.isEmpty() ? tr("The note file used for reordering was not found")
                                                    : tr("Failed to update the note order: %1").arg(file.errorString());
            file.close();
            rollBack();
            job->fail({ StorageError::Io, message, false });
            return job;
        }
        file.close();
        applied.append({ fileName, previousModified });
    }

    // One refresh is enough for the whole block and prevents observers from
    // rebuilding an intermediate order once per changed timestamp.
    emit invalidated();
    job->complete();
    return job;
}

void PTFStorage::removeNote(const QString &noteId)
{
    qCInfo(logPtfStorage) << "Removing PTF note idHash=" << diagnosticName(noteId);
    FileStorage::removeNote(noteId);
    if (!noteId.isEmpty())
        QDir(notesDir.filePath(noteId)).removeRecursively();
    if (folderCatalogAvailable_) {
        FolderCatalog catalog;
        if (!catalog.replaceSnapshot(folderCatalog_) && catalog.assignment(systemName(), noteId)) {
            if (const auto clearError = catalog.clearNoteAssignment(systemName(), noteId)) {
                qCWarning(logPtfStorage) << "Failed to clear PTF folder assignment:" << clearError.message;
            } else if (const auto persistError = replaceFolderCatalog(catalog.snapshot())) {
                qCWarning(logPtfStorage) << "Failed to persist cleared PTF folder assignment:" << persistError.message;
                emit storageErorr(tr("Failed to update the PTF folder index after removing a note."));
            }
        }
    }
    logDirectorySnapshot(notesDir, "PTF post-remove snapshot:");
}

QList<Note::Format> PTFStorage::availableFormats() const
{
    static auto formats = QList<Note::Format>() << Note::Markdown << Note::PlainText;
    return formats;
}

QString PTFStorage::findStorageDir() const { return Utils::qtnoteDataDir() + QLatin1Char('/') + storageId; }

FolderCatalogSnapshot PTFStorage::nativeFolderCatalog() const { return folderCatalog_; }

NoteFolderChangeJob *PTFStorage::changeNoteFolderAsync(const Note &note, QObject *owner)
{
    auto *job = new NoteFolderChangeJob(owner ? owner : this);
    job->start();
    if (note.isNull() || note.storage() != this || note.id().isEmpty()) {
        job->fail({ StorageError::NotFound, tr("The note used for the folder change was not found"), false });
        return job;
    }
    if (!noteFileExists(note.id())) {
        job->fail({ StorageError::NotFound, tr("The note used for the folder change no longer exists"), false });
        return job;
    }
    if (!folderCatalogAvailable_) {
        job->fail({ StorageError::Io,
                    folderCatalogError_.isEmpty() ? tr("The PTF folder index is unavailable") : folderCatalogError_,
                    false });
        return job;
    }

    FolderCatalog catalog;
    if (const auto validation = catalog.replaceSnapshot(folderCatalog_)) {
        job->fail({ StorageError::Other, validation.message, false });
        return job;
    }

    FolderCatalogError change;
    if (note.folderId().isNull()) {
        if (catalog.assignment(systemName(), note.id()))
            change = catalog.clearNoteAssignment(systemName(), note.id());
    } else {
        change = catalog.assignNote(systemName(), note.id(), note.folderId());
    }
    if (change) {
        job->fail({ change.code == FolderCatalogError::NotFound ? StorageError::NotFound : StorageError::Other,
                    change.message, false });
        return job;
    }
    if (const auto persistError = replaceFolderCatalog(catalog.snapshot())) {
        job->fail({ StorageError::Io, persistError.message, false });
        return job;
    }

    Note changed = note;
    emit noteModified(changed);
    job->complete(changed);
    return job;
}

FolderCatalogJob *PTFStorage::replaceNativeFolderCatalogAsync(const FolderCatalogSnapshot &snapshot, QObject *owner)
{
    auto *job = new FolderCatalogJob(owner ? owner : this);
    job->start();
    if (const auto replacementError = replaceFolderCatalog(snapshot)) {
        job->fail({ StorageError::Io, replacementError.message, false });
        return job;
    }
    job->complete();
    return job;
}

FolderCatalogError PTFStorage::restoreFolderCatalogBackup(QString *preservedPath)
{
    PtfFolderIndex index(notesDir.filePath(FolderIndexName), systemName());
    if (const auto restoreError = index.restoreBackup(preservedPath))
        return restoreError;
    loadFolderCatalog();
    if (!folderCatalogAvailable_)
        return { FolderCatalogError::Corrupt, folderCatalogError_ };
    emit invalidated();
    return {};
}

FolderCatalogError PTFStorage::recreateFolderCatalog(QString *preservedPath)
{
    PtfFolderIndex index(notesDir.filePath(FolderIndexName), systemName());
    if (const auto recreateError = index.recreate(preservedPath))
        return recreateError;
    loadFolderCatalog();
    if (!folderCatalogAvailable_)
        return { FolderCatalogError::Corrupt, folderCatalogError_ };
    emit invalidated();
    return {};
}

void PTFStorage::loadFolderCatalog()
{
    folderCatalog_          = {};
    folderCatalogAvailable_ = false;
    folderCatalogError_.clear();

    PtfFolderIndex index(notesDir.filePath(FolderIndexName), systemName());
    const auto     loaded = index.load();
    if (!loaded) {
        folderCatalogError_ = loaded.error.message;
        qCWarning(logPtfStorage) << "PTF folder index is unavailable:" << loaded.error.message
                                 << "backupAvailable=" << index.hasBackup();
        return;
    }
    folderCatalog_          = loaded.value;
    folderCatalogAvailable_ = true;
}

QUuid PTFStorage::folderIdForNote(const QString &noteId) const
{
    if (!folderCatalogAvailable_ || noteId.isEmpty())
        return {};
    for (const auto &assignment : folderCatalog_.assignments) {
        if (assignment.storageId == systemName() && assignment.noteId == noteId && !assignment.tombstone)
            return assignment.folderId;
    }
    return {};
}

FolderCatalogError PTFStorage::replaceFolderCatalog(const FolderCatalogSnapshot &snapshot)
{
    if (!folderCatalogAvailable_) {
        return { FolderCatalogError::Corrupt,
                 folderCatalogError_.isEmpty() ? tr("The PTF folder index is unavailable") : folderCatalogError_ };
    }

    FolderCatalogSnapshot owned;
    owned.folders = snapshot.folders;
    for (const auto &assignment : snapshot.assignments) {
        if (assignment.storageId == systemName())
            owned.assignments.append(assignment);
    }

    PtfFolderIndex index(notesDir.filePath(FolderIndexName), systemName());
    if (const auto saveError = index.save(owned)) {
        if (saveError.code == FolderCatalogError::Corrupt) {
            folderCatalog_          = {};
            folderCatalogAvailable_ = false;
            folderCatalogError_     = saveError.message;
        }
        return saveError;
    }
    folderCatalog_ = std::move(owned);
    return {};
}

FolderCatalogError PTFStorage::updateFolderAssignment(const QString &oldNoteId, const Note &saved)
{
    if (!folderCatalogAvailable_) {
        if (saved.folderId().isNull())
            return {};
        return { FolderCatalogError::Corrupt,
                 folderCatalogError_.isEmpty() ? tr("The PTF folder index is unavailable") : folderCatalogError_ };
    }

    FolderCatalog catalog;
    if (const auto validation = catalog.replaceSnapshot(folderCatalog_))
        return validation;

    const auto oldAssignment = oldNoteId.isEmpty() ? nullptr : catalog.assignment(systemName(), oldNoteId);
    const auto oldFolderId   = oldAssignment && !oldAssignment->tombstone ? oldAssignment->folderId : QUuid {};
    if (oldFolderId.isNull() && saved.folderId().isNull())
        return {};
    if (oldNoteId == saved.id() && oldFolderId == saved.folderId())
        return {};

    if (!oldNoteId.isEmpty() && oldAssignment) {
        if (const auto clearError = catalog.clearNoteAssignment(systemName(), oldNoteId))
            return clearError;
    }
    if (!saved.folderId().isNull()) {
        if (const auto assignError = catalog.assignNote(systemName(), saved.id(), saved.folderId()))
            return assignError;
    }
    return replaceFolderCatalog(catalog.snapshot());
}

QString PTFStorage::storageId = QStringLiteral("ptf");

} // namespace QtNote
