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

#include "notedata.h"
#include "notestorage.h"
#include "notetagline.h"

namespace QtNote {

NoteData::NoteData(NoteStorage *storage) : storage_(storage) { }

QString NoteData::storageId() const { return storage_ ? storage_->systemName() : QString(); }

QStringList NoteData::tags() const
{
    auto ret = tags_;
    for (const auto &tag : tagsFromText(text_)) {
        if (!ret.contains(tag)) {
            ret.append(tag);
        }
    }
    return ret;
}

QStringList NoteData::tagsFromLine(const QString &line) { return NoteTagLine::parseLine(line); }

QStringList NoteData::tagsFromText(const QString &text)
{
    const QString normalized = NoteTagLine::normalizeLineBreaks(text);
    const auto    idx        = normalized.indexOf(QLatin1Char('\n'));
    return tagsFromLine(idx == -1 ? normalized : normalized.left(idx));
}

void NoteData::setTags(const QStringList &tags) { tags_ = tags; }

}
