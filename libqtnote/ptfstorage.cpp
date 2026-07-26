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

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include "ptfstorage.h"
#include "utils.h"

namespace QtNote {
namespace {
    const QString MediaManifestName = QStringLiteral(".qtnote-media.json");

    struct SidecarMetadata {
        QUuid   id;
        QString originalName;
        QString mediaType;
    };

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
    if (!notesDir.exists()) {
        if (!notesDir.mkpath(QLatin1String("."))) {
            qWarning("can't create storage dir: %s", qPrintable(notesDir.absolutePath()));
        }
    }
    return isAccessible();
}

bool PTFStorage::isAccessible() const { return notesDir.isReadable(); }

const QString PTFStorage::systemName() const { return storageId; }

const QString PTFStorage::name() const { return tr("Plain Text Storage"); }

QIcon PTFStorage::storageIcon() const { return icon; }

QIcon PTFStorage::noteIcon() const { return icon; }

QList<Note> PTFStorage::noteListFromInfoList(const QFileInfoList &files)
{
    QList<Note> ret;
    foreach (const QFileInfo &fi, files) {
        // canonicalFilePath() can be empty for valid files exposed through
        // Android's app-specific external storage. The directory listing has
        // already resolved the entry, so its absolute path is the appropriate
        // stable backend path here.
        const QString filePath = fi.absoluteFilePath();
        QFile         file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        Note note(new NoteData(this));
        note.setId(fi.completeBaseName());
        note.setFormat(fi.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                          : Note::PlainText);
        note.setTitle(QString::fromUtf8(file.readLine()).trimmed());
        note.setTags(NoteData::tagsFromLine(QString::fromUtf8(file.readLine())));
        note.setLastChangeUTC(fi.lastModified());
        note.setBackendValue(QStringLiteral("fileName"), filePath);
        note.unload();
        ret.append(note);
    }
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
    if (!noteId.isEmpty()) {
        QFileInfo fi;
        for (auto const &ext : std::as_const(fileExt)) {
            fi = QFileInfo(QDir(notesDir).absoluteFilePath(QString("%1.%2").arg(noteId, ext)));
            if (fi.exists()) {
                Note loaded(new NoteData(this));
                loaded.setId(fi.completeBaseName());
                loaded.setFormat(fi.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) == 0 ? Note::Markdown
                                                                                                    : Note::PlainText);
                loaded.setLastChangeUTC(fi.lastModified());
                loaded.setBackendValue(QStringLiteral("fileName"), fi.filePath());
                if (loadNote(loaded))
                    return loaded;
                handleFSError();
                return {};
            }
        }
    }
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
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QString               contents = QString::fromUtf8(file.readAll());
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
    return true;
}

bool PTFStorage::saveNote(const Note &note)
{
    if (note.isEmpty()) {
        if (!note.id().isEmpty()) {
            removeNote(note.id()); // TODO check errors?
        }
        return true;
    }

    QString       oldNoteId = note.id();
    QString       newNoteId = oldNoteId;
    auto          ext       = QString(QLatin1String(note.format() == Note::Markdown ? "md" : "txt"));
    auto          fileName  = Utils::fileNameForText(notesDir, note.title(), ext, newNoteId);
    QString       contents  = note.title() + QLatin1Char('\n') + note.text();
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
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(bytes) != bytes.size() || !file.commit()) {
        handleFSError();
        return false;
    }
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
    saved.setBackendValue(QStringLiteral("fileName"), fileName);
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
    putToCache(saved, oldNoteId);
    return true;
}

void PTFStorage::removeNote(const QString &noteId)
{
    FileStorage::removeNote(noteId);
    if (!noteId.isEmpty())
        QDir(notesDir.filePath(noteId)).removeRecursively();
}

QList<Note::Format> PTFStorage::availableFormats() const
{
    static auto formats = QList<Note::Format>() << Note::Markdown << Note::PlainText;
    return formats;
}

QString PTFStorage::findStorageDir() const { return Utils::qtnoteDataDir() + QLatin1Char('/') + storageId; }

QString PTFStorage::storageId = QStringLiteral("ptf");

} // namespace QtNote
