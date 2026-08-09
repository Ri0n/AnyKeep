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

#ifndef HUNSPELLDOWNLOADER_H
#define HUNSPELLDOWNLOADER_H

#include "dictionarydownloader.h"
#include "spellcheckplugin.h"

#include <QLocale>

class QNetworkAccessManager;
class QNetworkReply;

#define COUNTRY_FN territory
#define COUNTRY_STR territoryToString

namespace AnyKeep {

class HunspellDownloader : public DictionaryDownloader {
    Q_OBJECT
public:
    explicit HunspellDownloader(const QLocale &locale, const QString &savePath, QNetworkAccessManager *nam,
                                QObject *parent = nullptr);
    void start() override;

private:
    void    findAndDownloadDictionary();
    QString findBestMatchingDirectory() const;

    const QString          savePath_;
    QNetworkAccessManager *nam_;
    QString                dicPath_;
    QString                affPath_;
    QStringList            cachedDirs_;
};

} // AnyKeep
#endif // HUNSPELLDOWNLOADER_H
