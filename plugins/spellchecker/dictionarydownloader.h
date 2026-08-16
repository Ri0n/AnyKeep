/*
AnyKeep - Simple note-taking application
Copyright (C) 2015 Sergei Ilinykh

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

#ifndef DICTIONARYDOWNLOADER_H
#define DICTIONARYDOWNLOADER_H

#include <QLocale>

namespace AnyKeep {

class DictionaryDownloader : public QObject {
    Q_OBJECT
public:
    DictionaryDownloader(const QLocale &locale, QObject *parent) : QObject(parent), locale_(locale) {}

    virtual void start() = 0;

    void           setAutoDelete(bool enabled) { autoDelete_ = enabled; }
    bool           hasErrors() const { return !lastError_.isEmpty(); }
    const QString &lastError() const { return lastError_; }
signals:
    void finished();
    void progress(int progress); // 0..100

protected:
    bool          autoDelete_ = false;
    const QLocale locale_;
    QString       lastError_;
};

} // namespace AnyKeep

#endif // DICTIONARYDOWNLOADER_H
