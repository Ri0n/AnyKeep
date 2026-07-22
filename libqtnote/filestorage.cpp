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

#include "filestorage.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QUuid>

#include "settingscontroller.h"

namespace QtNote {

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

FileStorage::FileStorage(QObject *parent) : NoteStorage(parent), _cacheValid(false) { }

void FileStorage::removeNote(const QString &noteId)
{
    auto r = cache.find(noteId);
    if (r == cache.end()) {
        return;
    }
    bool deleted = false;
    for (auto const &ext : std::as_const(fileExt)) {
        if (notesDir.remove(QString("%1.%2").arg(noteId, ext))) {
            deleted = true;
        }
    }
    if (deleted) {
        Note item = r.value();
        cache.erase(r);
        emit noteRemoved(item);
    }
}

void FileStorage::handleFSError()
{
    cache.clear();
    _cacheValid = false;
    emit storageErorr(tr("File system error for storage \"%1\". Please check your settings.").arg(name()));
    emit invalidated();
}

void FileStorage::putToCache(const Note &note, const QString &oldNoteId)
{
    bool isModify;
    bool idChange = false;

    ensureChachePopulated();

    if (!oldNoteId.isEmpty() && oldNoteId != note.id()) {
        isModify = cache.contains(oldNoteId);
        idChange = true;
    } else {
        isModify = cache.contains(note.id());
    }

    cache.insert(note.id(), note);
    if (isModify) {
        if (idChange) {
            cache.remove(oldNoteId);
            emit noteIdChanged(note, oldNoteId);
        }
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
    cache.clear();
    _cacheValid = false;
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

void FileStorage::ensureChachePopulated()
{
    if (_cacheValid) {
        return;
    }
    if (isAccessible()) {
        QStringList wcards;
        // could be done with ranges but see https://bugreports.qt.io/browse/QTBUG-120924
        for (auto const &ext : std::as_const(fileExt)) {
            wcards.append(QLatin1String("*.") + ext);
        }

        QFileInfoList files = notesDir.entryInfoList(wcards, QDir::Files, QDir::Time);
        auto          ret   = noteListFromInfoList(files);
        cache.clear();
        for (auto &n : ret) {
            cache.insert(n.id(), n);
        }
        _cacheValid = true;
    } else {
        handleFSError();
    }
}

QList<Note> FileStorage::noteList(int limit)
{
    ensureChachePopulated();
    QList<Note> ret = cache.values();
    std::sort(ret.begin(), ret.end(), noteListItemModifyComparer);
    // probably sort is unnecesary here if the only accessor is notemanager which also does sorting.
    return limit ? ret.mid(0, limit) : ret;
}

} // namespace QtNote
