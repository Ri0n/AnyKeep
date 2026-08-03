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

#include "filestorage.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QSettings>
#include <QUuid>

#include "settingscontroller.h"

#include <algorithm>
#include <utility>

namespace AnyKeep {

Q_LOGGING_CATEGORY(logFileStorage, "anykeep.persistence.file")

class FileStorageSettingsController final : public SettingsController {
public:
    explicit FileStorageSettingsController(FileStorage *storage, QObject *parent = nullptr) :
        SettingsController(parent), storage_(storage)
    {
        const QString customPath = storage_->customStoragePath();
        QList<Field>  fields;
        Field         custom;
        custom.key   = QStringLiteral("customPathEnabled");
        custom.label = tr("Use a custom storage directory");
        custom.type  = Boolean;
        custom.value = !customPath.isEmpty();
        fields.append(custom);

        Field path;
        path.key         = QStringLiteral("path");
        path.label       = tr("Storage directory");
        path.description = tr(
            "Enter an existing writable directory. Disable the custom directory option to use the platform default.");
        path.type        = Text;
        path.value       = customPath.isEmpty() ? storage_->findStorageDir() : customPath;
        path.placeholder = storage_->findStorageDir();
        fields.append(path);
        setFields(std::move(fields));
    }

protected:
    bool applyValues(const QVariantMap &values, QString *error) override
    {
        const bool    custom = values.value(QStringLiteral("customPathEnabled")).toBool();
        const QString path
            = custom ? values.value(QStringLiteral("path")).toString().trimmed() : storage_->findStorageDir();
        if (!storage_->setStoragePath(path)) {
            if (error)
                *error = tr("The selected directory does not exist or is not writable.");
            return false;
        }
        return true;
    }

private:
    FileStorage *storage_;
};

FileStorage::FileStorage(QObject *parent) : NoteStorage(parent) { }

void FileStorage::removeNote(const QString &noteId)
{
    QFileInfoList matchingFiles;
    for (const auto &ext : std::as_const(fileExt)) {
        const QFileInfo fileInfo(notesDir.absoluteFilePath(QStringLiteral("%1.%2").arg(noteId, ext)));
        if (fileInfo.exists())
            matchingFiles.append(fileInfo);
    }

    Note       removed;
    const auto summaries = noteListFromInfoList(matchingFiles);
    if (!summaries.isEmpty())
        removed = summaries.first();

    bool deleted = false;
    for (const auto &fileInfo : std::as_const(matchingFiles)) {
        if (QFile::remove(fileInfo.absoluteFilePath()))
            deleted = true;
    }
    if (!deleted) {
        emit invalidated();
        return;
    }
    if (!removed.isNull())
        emit noteRemoved(removed);
    else
        emit invalidated();
}

void FileStorage::handleFSError()
{
    qCWarning(logFileStorage) << "Filesystem error in storage" << systemName() << "path=" << notesDir.absolutePath();
    emit storageErorr(tr("File system error for storage \"%1\". Please check your settings.").arg(name()));
    emit invalidated();
}

bool FileStorage::noteFileExists(const QString &noteId) const
{
    if (noteId.isEmpty())
        return false;
    for (const auto &ext : std::as_const(fileExt)) {
        if (QFileInfo::exists(notesDir.absoluteFilePath(QStringLiteral("%1.%2").arg(noteId, ext))))
            return true;
    }
    return false;
}

void FileStorage::notifyNoteSaved(const Note &note, const QString &oldNoteId, bool existedBeforeSave)
{
    if (existedBeforeSave) {
        if (!oldNoteId.isEmpty() && oldNoteId != note.id())
            emit noteIdChanged(note, oldNoteId);
        emit noteModified(note);
    } else {
        emit noteAdded(note);
    }
}

QString FileStorage::customStoragePath() const
{
    return QSettings().value(QStringLiteral("storage.%1.path").arg(systemName())).toString();
}

bool FileStorage::setStoragePath(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }
    QFileInfo fi(path);
    if (!fi.isDir() || !fi.isWritable()) {
        return false;
    }
    const auto absolutePath = fi.absoluteFilePath();
    notesDir.setPath(absolutePath);
    QSettings().setValue(QStringLiteral("storage.%1.path").arg(systemName()),
                         notesDir.absolutePath() == findStorageDir() ? QString() : absolutePath);
    init();
    emit invalidated();
    return true;
}

QUrl FileStorage::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")); }

SettingsController *FileStorage::createSettingsController(QObject *parent)
{
    return new FileStorageSettingsController(this, parent);
}

QString FileStorage::tooltip() { return QString("<b>%1:</b> %2").arg(tr("Storage path"), notesDir.absolutePath()); }

QList<Note> FileStorage::noteList(int limit)
{
    if (!isAccessible()) {
        handleFSError();
        return {};
    }

    QStringList wildcards;
    // Could be done with ranges but see https://bugreports.qt.io/browse/QTBUG-120924
    for (const auto &ext : std::as_const(fileExt))
        wildcards.append(QLatin1String("*.") + ext);

    const auto files = notesDir.entryInfoList(wildcards, QDir::Files, QDir::Time);
    qCInfo(logFileStorage) << "Scanning file storage: storage=" << systemName() << "path=" << notesDir.absolutePath()
                           << "filters=" << wildcards << "entries=" << files.size();
    auto notes = noteListFromInfoList(files);
    std::sort(notes.begin(), notes.end(), noteListItemModifyComparer);
    return limit > 0 ? notes.mid(0, limit) : notes;
}

} // namespace AnyKeep
