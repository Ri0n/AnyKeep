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

#ifndef FILESTORAGE_H
#define FILESTORAGE_H

#include <QDir>
#include <QFileInfoList>
#include <QHash>

#include "notestorage.h"
#include "qtnote_export.h"

namespace QtNote {

class QTNOTE_EXPORT FileStorage : public NoteStorage {
    Q_OBJECT
public:
    FileStorage(QObject *parent);
    void                removeNote(const QString &noteId) override;
    QString             tooltip() override;
    QList<Note>         noteList(int limit = 0) override;
    virtual QString     findStorageDir() const = 0;
    QString             customStoragePath() const;
    bool                setStoragePath(const QString &path);
    bool                isConfigurable() const override { return true; }
    QUrl                settingsComponent() const override;
    SettingsController *createSettingsController(QObject *parent = nullptr) override;

protected:
    virtual QList<Note> noteListFromInfoList(const QFileInfoList &) = 0;

    void putToCache(const Note &note, const QString &oldNoteId);

    void handleFSError();
    void ensureChachePopulated();

protected:
    QStringList          fileExt;
    QHash<QString, Note> cache;
    bool                 _cacheValid; /* last limit passed to noteList() */
    QDir                 notesDir;
};

}

#endif // FILESTORAGE_H
