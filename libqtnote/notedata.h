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

#ifndef NOTEDATA_H
#define NOTEDATA_H

#include <QPointer>
#include <QSharedData>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariantMap>

#include "note.h"

namespace QtNote {

class NoteStorage;

class QTNOTE_EXPORT NoteData : public QSharedData {
public:
    static constexpr int TitleLength = 256;

    explicit NoteData(NoteStorage *storage);
    ~NoteData() = default;

    QString storageId() const;

    void setId(const QString &newId) { id_ = newId; }
    void unload()
    {
        text_   = QString(); // should reset capacity
        loaded_ = false;
    }
    void setText(const QString &text, Note::Format format)
    {
        loaded_ = true;
        text_   = text;
        format_ = format;
        tags_   = tagsFromText(text_);
    }

    QStringList tags() const;
    QVariant    backendValue(const QString &key) const { return backendData_.value(key); }
    void        setBackendValue(const QString &key, const QVariant &value) { backendData_.insert(key, value); }
    void        removeBackendValue(const QString &key) { backendData_.remove(key); }

    static QStringList tagsFromLine(const QString &line);
    static QStringList tagsFromText(const QString &text);

protected:
    void setTags(const QStringList &tags);

    friend class Note;
    friend class NoteStorage;
    QPointer<NoteStorage> storage_;
    bool                  loaded_ { false };
    Note::Format          format_; // format of the text_ field
    QString               id_;     // this can match with title sometimes
    QString               title_;
    QString               text_;
    QStringList           tags_;
    QUuid                 folderId_;
    QDateTime             lastChange_;
    QVariantMap           backendData_;
    QList<MediaReference> media_;
};

} // namespace QtNote

#endif // NOTEDATA_H
