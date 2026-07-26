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

#ifndef PTFSTORAGE_H
#define PTFSTORAGE_H

#include "filestorage.h"

namespace QtNote {

class QTNOTE_EXPORT PTFStorage : public FileStorage {
    Q_OBJECT
    Q_DISABLE_COPY(PTFStorage)

    QIcon icon;

public:
    PTFStorage(QObject *parent = 0);
    bool          init() override;
    bool          isAccessible() const override;
    const QString systemName() const override;
    const QString name() const override;
    QIcon         storageIcon() const override;
    QIcon         noteIcon() const override;
    QList<Note>   noteListFromInfoList(const QFileInfoList &) override;

    Note createNote() override;
    Note note(const QString &noteId) override;
    bool loadNote(Note &note) override;
    bool saveNote(const Note &note) override;
    void removeNote(const QString &noteId) override;

    QList<Note::Format> availableFormats() const override;
    bool                supportsMedia() const override { return true; }
    QString             findStorageDir() const override;

    static QString storageId;
};

} // namespace QtNote

#endif // PTFSTORAGE_H
