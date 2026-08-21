/*
AnyKeep - Simple note-taking application
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
#include <utility>

#ifdef Q_OS_WIN
#include <QLibrary>
#include <shlobj.h>
#endif // Q_OS_WIN

#include "localmediastore.h"
#include "notedata.h"
#include "notetitleresolver.h"
#include "ptffolderindex.h"
#include "ptfstorage.h"
#include "utils.h"

namespace AnyKeep {

Q_LOGGING_CATEGORY(logPtfStorage, "anykeep.persistence.ptf")

namespace {
    const QString MediaManifestName       = QStringLiteral(".anykeep-media.json");
    const QString FolderIndexName         = QStringLiteral(".anykeep-folders.json");
    const QString LegacyMediaManifestName = QStringLiteral(".qtnote-media.json");
    const QString LegacyFolderIndexName   = QStringLiteral(".qtnote-folders.json");

    std::pair<QString, QString> splitPtfContents(QString contents)
    {
        if (contents.startsWith(QChar::ByteOrderMark))
            contents.remove(0, 1);
        contents.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        contents.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        const qsizetype separator = contents.indexOf(QLatin1Char('\n'));
        if (separator < 0)
            return { contents, {} };
        return { contents.left(separator), contents.mid(separator + 1) };
    }

    bool copyDirectoryContents(const QDir &source, const QDir &destination)
    {
        if (!destination.exists() && !QDir().mkpath(destination.absolutePath()))
            return false;
        const auto entries = source.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        for (const auto &entry : entries) {
            const auto targetPath = destination.filePath(entry.fileName());
            if (entry.isDir()) {
                if (!copyDirectoryContents(QDir(entry.absoluteFilePath()), QDir(targetPath)))
                    return false;
            } else if (!QFileInfo::exists(targetPath) && !QFile::copy(entry.absoluteFilePath(), targetPath)) {
                return false;
            }
        }
        return true;
    }

    bool replaceLegacyMarkup(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        auto contents = QString::fromUtf8(file.readAll());
        file.close();
        const auto original = contents;
        contents.replace(QStringLiteral("qtnote-media:"), QStringLiteral("anykeep-media:"), Qt::CaseInsensitive);
        contents.replace(QStringLiteral("data-qtnote-"), QStringLiteral("data-anykeep-"), Qt::CaseInsensitive);
        contents.replace(QStringLiteral("<!-- qtnote:"), QStringLiteral("<!-- anykeep:"), Qt::CaseInsensitive);
        if (contents == original)
            return true;
        QSaveFile replacement(path);
        return replacement.open(QIODevice::WriteOnly | QIODevice::Text) && replacement.write(contents.toUtf8()) >= 0
            && replacement.commit();
    }

    void migrateLegacySidecars(const QDir &notesDir)
    {
        const auto renameIfNeeded = [&notesDir](const QString &legacyName, const QString &currentName) {
            const auto legacyPath  = notesDir.filePath(legacyName);
            const auto currentPath = notesDir.filePath(currentName);
            if (QFileInfo::exists(legacyPath) && !QFileInfo::exists(currentPath))
                QFile::rename(legacyPath, currentPath);
        };
        renameIfNeeded(LegacyFolderIndexName, FolderIndexName);

        const auto notes = notesDir.entryInfoList({ QStringLiteral("*.md") }, QDir::Files);
        for (const auto &note : notes)
            replaceLegacyMarkup(note.absoluteFilePath());

        const auto sidecars = notesDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &sidecar : sidecars) {
            const QDir sidecarDir(sidecar.absoluteFilePath());
            const auto legacyPath  = sidecarDir.filePath(LegacyMediaManifestName);
            const auto currentPath = sidecarDir.filePath(MediaManifestName);
            if (QFileInfo::exists(legacyPath) && !QFileInfo::exists(currentPath))
                QFile::rename(legacyPath, currentPath);
        }
    }

    void migrateLegacyPtfStorage(QDir &notesDir, bool usesDefaultPath)
    {
#ifndef Q_OS_ANDROID
        if (usesDefaultPath && !notesDir.exists()) {
            const QDir legacyRoot(Utils::genericDataDir() + QStringLiteral("/R-Soft/QtNote"));
            const QDir legacyNotes(legacyRoot.filePath(PTFStorage::storageId));
            if (legacyNotes.exists()) {
                copyDirectoryContents(legacyNotes, notesDir);
                const QDir legacyMedia(legacyRoot.filePath(QStringLiteral("media")));
                if (legacyMedia.exists())
                    copyDirectoryContents(legacyMedia, QDir(Utils::anykeepDataDir() + QStringLiteral("/media")));
            }
        }
#else
        Q_UNUSED(usesDefaultPath)
#endif
        if (notesDir.exists())
            migrateLegacySidecars(notesDir);
    }

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

    QString decodeHtmlAttribute(QString value)
    {
        value.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
        value.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
        value.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
        value.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
        value.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        return value;
    }

    QString importSidecarMedia(QString body, const QString &noteId, const QDir &notesDir, LocalMediaStore &mediaStore,
                               QList<MediaReference> &media)
    {
        const QDir sidecar(notesDir.filePath(noteId));
        if (!sidecar.exists())
            return body;
        const auto metadata = readMediaMetadata(sidecar);

        struct SourceMatch {
            qsizetype start;
            qsizetype length;
            QString   target;
        };
        QList<SourceMatch> sources;

        static const QRegularExpression markdownImage(QStringLiteral(R"(!\[([^\]]*)\]\(([^\s\)]+)(?:\s+"[^"]*")?\))"));
        auto                            markdownMatches = markdownImage.globalMatch(body);
        while (markdownMatches.hasNext()) {
            const auto match = markdownMatches.next();
            sources.append({ match.capturedStart(2), match.capturedLength(2), match.captured(2) });
        }

        // Resized/non-centred images, audio, and generic attachments use HTML
        // blocks. Restore src/href targets through the encrypted media store.
        static const QRegularExpression htmlMedia(
            QStringLiteral(
                R"(<(?:img|audio)\b[^>]*?\s+src\s*=\s*(["'])(.*?)\1[^>]*>|<a\b[^>]*?\s+href\s*=\s*(["'])(.*?)\3[^>]*\bdata-anykeep-attachment\b[^>]*>)"),
            QRegularExpression::CaseInsensitiveOption);
        auto htmlMatches = htmlMedia.globalMatch(body);
        while (htmlMatches.hasNext()) {
            const auto match         = htmlMatches.next();
            const int  targetCapture = match.captured(2).isNull() ? 4 : 2;
            sources.append({ match.capturedStart(targetCapture), match.capturedLength(targetCapture),
                             decodeHtmlAttribute(match.captured(targetCapture)) });
        }

        std::sort(sources.begin(), sources.end(),
                  [](const SourceMatch &left, const SourceMatch &right) { return left.start < right.start; });

        struct Replacement {
            qsizetype start;
            qsizetype length;
            QString   value;
        };
        QList<Replacement> replacements;
        qsizetype          previousEnd = -1;
        for (const auto &source : std::as_const(sources)) {
            if (source.start < previousEnd)
                continue;
            previousEnd = source.start + source.length;

            const auto rawTarget = source.target;
            QString    mediaName;
            QUuid      uriId;
            if (rawTarget.startsWith(QStringLiteral("anykeep-media:/"), Qt::CaseInsensitive)) {
                const QUrl targetUrl(rawTarget);
                const auto parts = targetUrl.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
                if (parts.size() != 2)
                    continue;
                uriId     = QUuid(parts.at(0));
                mediaName = parts.at(1);
                if (uriId.isNull())
                    continue;
            } else {
                const auto      target = QUrl::fromPercentEncoding(rawTarget.toUtf8());
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

            const auto alreadyImported = std::find_if(
                media.cbegin(), media.cend(), [&uriId, &mediaName](const auto &item) {
                    return (!uriId.isNull() && item.id == uriId) || (uriId.isNull() && item.portableName == mediaName);
                });
            if (alreadyImported != media.cend()) {
                replacements.prepend({ source.start, source.length, alreadyImported->uri() });
                continue;
            }

            const auto item     = metadata.value(mediaName);
            auto       imported = mediaStore.importFile(sidecar.filePath(mediaName), uriId.isNull() ? item.id : uriId);
            if (!imported)
                continue;
            if (!item.originalName.isEmpty())
                imported.value.originalName = item.originalName;
            if (!item.mediaType.isEmpty())
                imported.value.mediaType = item.mediaType;
            media.append(imported.value);
            replacements.prepend({ source.start, source.length, imported.value.uri() });
        }
        for (const auto &replacement : std::as_const(replacements))
            body.replace(replacement.start, replacement.length, replacement.value);
        return body;
    }

} // namespace

PTFStorage::PTFStorage(QObject *parent) : PTFStorage(*LocalMediaStore::instance(), parent) {}

PTFStorage::PTFStorage(LocalMediaStore &mediaStore, QObject *parent) :
    FileStorage(parent), icon(QLatin1String(":/icons/trayicon")), mediaStore_(mediaStore)
{
    fileExt.append(QLatin1String("txt"));
    fileExt.append(QLatin1String("md"));
}

bool PTFStorage::init()
{
    bool usesDefaultPath = true;
#ifdef Q_OS_ANDROID
    // The mobile storage is always private application data. A stale or
    // manually supplied desktop path must not redirect it to shared storage.
    notesDir.setPath(findStorageDir());
#else
    QSettings settings;
    auto      path = settings.value("storage.ptf.path").toString();
    if (path.isEmpty()) {
        const QSettings legacySettings(QStringLiteral("R-Soft"), QStringLiteral("QtNote"));
        path = legacySettings.value(QStringLiteral("storage.ptf.path")).toString();
        if (!path.isEmpty())
            settings.setValue(QStringLiteral("storage.ptf.path"), path);
    }
    usesDefaultPath = path.isEmpty();
    notesDir.setPath(path.isEmpty() ? findStorageDir() : path);
    if (!notesDir.isReadable() && !path.isEmpty()) {
        notesDir.setPath(findStorageDir()); // try default
        usesDefaultPath = true;
    }
#endif
    migrateLegacyPtfStorage(notesDir, usesDefaultPath);
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
        note.setFavorite(favoriteNoteIds_.contains(note.id()));
        note.setFormat(fi.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                          : Note::PlainText);
        const auto [title, body] = splitPtfContents(QString::fromUtf8(file.readAll()));
        note.setTitle(title);
        note.setTags(NoteData::tagsFromText(body));
        note.setBackendValue(QString::fromLatin1(NoteTitleResolver::CachedDisplayTitleBackendKey),
                             NoteTitleResolver::displayTitle(title, body, note.format()));
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
                loaded.setFavorite(favoriteNoteIds_.contains(loaded.id()));
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
        contents = importSidecarMedia(contents, QFileInfo(fileName).completeBaseName(), notesDir, mediaStore_, media);
    auto [title, body] = splitPtfContents(contents);
    note.setTitle(title);
    note.setText(body,
                 QFileInfo(fileName).suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                                     : Note::PlainText);
    note.setLastChangeUTC(QFileInfo(file).lastModified());
    note.setBackendValue(QStringLiteral("fileName"), fileName);
    note.setBackendValue(QString::fromLatin1(NoteTitleResolver::CachedDisplayTitleBackendKey),
                         NoteTitleResolver::displayTitle(title, body, note.format()));
    note.setMedia(media);
    note.setFolderId(folderIdForNote(note.id()));
    note.setFavorite(favoriteNoteIds_.contains(note.id()));
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
    QString    displayTitle      = NoteTitleResolver::displayTitle(note.title(), note.text(), note.format());
    if (displayTitle.isEmpty())
        displayTitle = tr("Untitled note");
    auto fileName = Utils::fileNameForText(notesDir, displayTitle, ext, newNoteId);
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
            const auto loaded = mediaStore_.data(reference.blobId);
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
    const auto bytes = contents.toUtf8();
    if (!file.open(QIODevice::WriteOnly)) {
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
    if (const auto metadataError = updateNoteMetadata(oldNoteId, saved)) {
        qCWarning(logPtfStorage) << "Failed to update PTF portable metadata:" << metadataError.message;
        emit storageErorr(tr("Failed to update PTF note metadata. The note content was saved, but its folder or "
                             "favorite state may need recovery."));
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
        if (!catalog.replaceSnapshot(folderCatalog_)) {
            auto favorites = favoriteNoteIds_;
            bool changed   = favorites.remove(noteId);
            if (catalog.assignment(systemName(), noteId)) {
                if (const auto clearError = catalog.clearNoteAssignment(systemName(), noteId)) {
                    qCWarning(logPtfStorage) << "Failed to clear PTF folder assignment:" << clearError.message;
                } else {
                    changed = true;
                }
            }
            if (changed) {
                if (const auto persistError = persistPortableMetadata(catalog.snapshot(), favorites)) {
                    qCWarning(logPtfStorage)
                        << "Failed to persist PTF metadata after removing note:" << persistError.message;
                    emit storageErorr(tr("Failed to update PTF note metadata after removing a note."));
                }
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

QString PTFStorage::findStorageDir() const { return Utils::anykeepDataDir() + QLatin1Char('/') + storageId; }

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
    folderCatalog_ = {};
    favoriteNoteIds_.clear();
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
    const auto favorites = index.loadFavoriteNoteIds();
    if (!favorites) {
        folderCatalogError_ = favorites.error.message;
        qCWarning(logPtfStorage) << "PTF favorite index is unavailable:" << favorites.error.message
                                 << "backupAvailable=" << index.hasBackup();
        return;
    }
    folderCatalog_          = loaded.value;
    favoriteNoteIds_        = favorites.value;
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
    return persistPortableMetadata(snapshot, favoriteNoteIds_);
}

FolderCatalogError PTFStorage::persistPortableMetadata(const FolderCatalogSnapshot &snapshot,
                                                       const QSet<QString>         &favoriteNoteIds)
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
    if (const auto saveError = index.save(owned, favoriteNoteIds)) {
        if (saveError.code == FolderCatalogError::Corrupt) {
            folderCatalog_ = {};
            favoriteNoteIds_.clear();
            folderCatalogAvailable_ = false;
            folderCatalogError_     = saveError.message;
        }
        return saveError;
    }
    folderCatalog_   = std::move(owned);
    favoriteNoteIds_ = favoriteNoteIds;
    return {};
}

FolderCatalogError PTFStorage::updateNoteMetadata(const QString &oldNoteId, const Note &saved)
{
    const bool favoriteRequiresIndex
        = saved.isFavorite() || (!oldNoteId.isEmpty() && favoriteNoteIds_.contains(oldNoteId));
    if (!folderCatalogAvailable_) {
        if (saved.folderId().isNull() && !favoriteRequiresIndex)
            return {};
        return { FolderCatalogError::Corrupt,
                 folderCatalogError_.isEmpty() ? tr("The PTF folder index is unavailable") : folderCatalogError_ };
    }

    FolderCatalog catalog;
    if (const auto validation = catalog.replaceSnapshot(folderCatalog_))
        return validation;

    bool       changed       = false;
    const auto oldAssignment = oldNoteId.isEmpty() ? nullptr : catalog.assignment(systemName(), oldNoteId);
    const auto oldFolderId   = oldAssignment && !oldAssignment->tombstone ? oldAssignment->folderId : QUuid {};
    if (oldNoteId != saved.id() || oldFolderId != saved.folderId()) {
        if (!oldNoteId.isEmpty() && oldAssignment) {
            if (const auto clearError = catalog.clearNoteAssignment(systemName(), oldNoteId))
                return clearError;
            changed = true;
        }
        if (!saved.folderId().isNull()) {
            if (const auto assignError = catalog.assignNote(systemName(), saved.id(), saved.folderId()))
                return assignError;
            changed = true;
        }
    }

    auto favorites = favoriteNoteIds_;
    if (!oldNoteId.isEmpty() && oldNoteId != saved.id())
        changed = favorites.remove(oldNoteId) || changed;
    const bool wasFavorite = favorites.contains(saved.id());
    if (saved.isFavorite())
        favorites.insert(saved.id());
    else
        favorites.remove(saved.id());
    changed = changed || (wasFavorite != saved.isFavorite());

    if (!changed)
        return {};
    return persistPortableMetadata(catalog.snapshot(), favorites);
}

QString PTFStorage::storageId = QStringLiteral("ptf");

} // namespace AnyKeep
