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

#ifndef NOTE_H
#define NOTE_H

#include <QExplicitlySharedDataPointer>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariant>
#include <QVariantMap>
#include <qdatetime.h>

#include "mediareference.h"
#include "qtnote_export.h"

namespace QtNote {

inline constexpr auto RequestedModificationTimeBackendKey = "qtnote.requestedModificationTime";

class NoteData;
class NoteStorage;

class QTNOTE_EXPORT Note {
public:
    enum Format { Markdown, PlainText, Html };

    Note();
    ~Note();
    Note(NoteData *data);
    Note(const Note &note);
    Note(Note &&note);
    Note &operator=(const Note &note);

    bool isNull() const;
    bool isEmpty() const;
    bool isLoaded() const;

    bool save();
    bool load(); // by default nothing is loaded (i.e. text() returns a null string)
    void remove();

    void setTitle(const QString &title);
    void setText(const QString &text, Format format);
    void setId(const QString &id);
    void setFormat(Format format);
    void setTags(const QStringList &tags);
    void setFolderId(const QUuid &folderId);
    void unload();
    void setLastChangeUTC(const QDateTime &lastChange);
    void setBackendValue(const QString &key, const QVariant &value);
    void removeBackendValue(const QString &key);

    NoteStorage          *storage() const;
    QString               storageId() const;
    QString               id() const;
    QString               text() const;
    QString               title() const;
    QString               displayTitle() const;
    QStringList           tags() const;
    QUuid                 folderId() const;
    NoteData             *data() const;
    Format                format() const;
    QDateTime             lastChangeUTC() const;
    QVariant              backendValue(const QString &key) const;
    QVariantMap           backendData() const;
    void                  setBackendData(const QVariantMap &values);
    QList<MediaReference> media() const;
    void                  setMedia(const QList<MediaReference> &media);
    bool                  isUpdated() const;

private:
    QExplicitlySharedDataPointer<NoteData> d;
};

inline bool noteListItemModifyComparer(const Note &a, const Note &b)
{
    return a.lastChangeUTC() > b.lastChangeUTC(); // backward order
}

} // namespace QtNote

#endif // NOTE_H
