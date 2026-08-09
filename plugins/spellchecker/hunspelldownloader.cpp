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

#include "hunspelldownloader.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSaveFile>

#include <memory>

namespace AnyKeep {

class DictListFetcher : public QObject {
    Q_OBJECT

    QString                savePath_;
    QNetworkAccessManager *nam_;
    QString                lastError_;
    QDateTime              cacheDateTime_;

public:
    DictListFetcher(const QString &savePath, QNetworkAccessManager *nam, QObject *parent) :
        QObject(parent), savePath_(savePath), nam_(nam)
    {
    }

    void start()
    {
        auto dirs = loadCachedDirectoryList();
        if (dirs.empty()) {
            QUrl dictionariesUrl(QLatin1String("https://cgit.freedesktop.org/libreoffice/dictionaries/plain/"));
            QNetworkReply *reply = nam_->get(QNetworkRequest(dictionariesUrl));
            connect(reply, &QNetworkReply::finished, this, [this, reply]() { onDirectoryListReceived(reply); });
            reply->setParent(this);
        } else {
            emit finished(dirs);
            deleteLater();
        }
    }

    QStringList loadCachedDirectoryList()
    {
        QFile cacheFile(savePath_ + QLatin1String("/directory_cache.json"));
        if (!cacheFile.open(QIODevice::ReadOnly)) {
            if (cacheFile.exists()) {
                qWarning("Failed to load cached directory list: %s", qUtf8Printable(cacheFile.errorString()));
            }
            // A missing cache is normal on first run. In either case the
            // closed QFile must not be read; start() will fetch a fresh list.
            return {};
        }

        QJsonDocument doc         = QJsonDocument::fromJson(cacheFile.readAll());
        auto          obj         = doc.object();
        auto          ret         = obj[QLatin1String("directories")].toVariant().toStringList();
        QDateTime     lastUpdated = QDateTime::fromString(doc[QLatin1String("last_updated")].toString(), Qt::ISODate);

        if (!lastUpdated.isValid() || lastUpdated.daysTo(QDateTime::currentDateTime()) > 1) {
            return {};
        }
        return ret;
    }

    void onDirectoryListReceived(QNetworkReply *reply)
    {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            lastError_ = QLatin1String("Failed to fetch a list of dictioneries: ") + reply->errorString();
            emit finished({});
            deleteLater();
            return;
        }

        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 304) {
            loadCachedDirectoryList();
            return;
        }

        auto dirs = parseDirectoryListing(reply->readAll());
        cacheDirectoryList(dirs);
        emit finished(dirs);
        deleteLater();
    }

    QStringList parseDirectoryListing(const QByteArray &data)
    {
        QStringList dirs;
        QTextStream stream(data);
        QString     line;

        const QString hrefPrefix    = QLatin1String("href='/libreoffice/dictionaries/plain/");
        const int     hrefPrefixLen = hrefPrefix.length();

        while (stream.readLineInto(&line)) {
            int hrefPos = line.indexOf(hrefPrefix);
            if (hrefPos == -1)
                continue;

            int start = hrefPos + hrefPrefixLen;
            int end   = line.indexOf(QLatin1Char('\''), start);
            if (end == -1)
                continue;

            QString item = line.mid(start, end - start);

            if (item.endsWith(QLatin1Char('/'))) {
                item.chop(1);
                dirs << item;
            }
        }

        return dirs;
    }

    void cacheDirectoryList(const QStringList &dirs)
    {
        QJsonObject cache;
        cache[QLatin1String("last_updated")] = QDateTime::currentDateTime().toString(Qt::ISODate);
        cache[QLatin1String("directories")]  = QJsonArray::fromStringList(dirs);

        QFile cacheFile(savePath_ + QLatin1String("/directory_cache.json"));
        if (cacheFile.open(QIODevice::WriteOnly)) {
            cacheFile.write(QJsonDocument(cache).toJson());
        }
    }

    const QString &lastError() const { return lastError_; }

signals:
    void finished(const QStringList &);
};

class AffDicDownloader : public QObject {
    Q_OBJECT

    const QString                savePath_;
    const QString                baseUrl_;
    QNetworkAccessManager *const nam_;

    QNetworkReply *dicReply_ = nullptr;
    QNetworkReply *affReply_ = nullptr;

    QSaveFile dicFile_;
    QSaveFile affFile_;
    QString   lastError_;

public:
    AffDicDownloader(const QString &savePath, const QString &baseUrl, QNetworkAccessManager *nam,
                     QObject *parent = nullptr) : QObject(parent), savePath_(savePath), baseUrl_(baseUrl), nam_(nam)
    {
        QDir().mkpath(savePath_);
        QString baseName = savePath + QLatin1Char('/') + QFileInfo(baseUrl).baseName();
        dicFile_.setFileName(baseName + QLatin1String(".dic"));
        affFile_.setFileName(baseName + QLatin1String(".aff"));
    }

    void start()
    {
        lastError_.clear();

        if (!dicFile_.open(QIODevice::WriteOnly) || !affFile_.open(QIODevice::WriteOnly)) {
            auto &failed = dicFile_.isOpen() ? affFile_ : dicFile_;
            lastError_
                = QLatin1String("Failed to open %1 for writing: %2").arg(failed.fileName(), failed.errorString());
            qDebug() << lastError_;
            emit finished();
            deleteLater();
            return;
        }

        dicReply_ = nam_->get(QNetworkRequest(QUrl(baseUrl_ + ".dic")));
        affReply_ = nam_->get(QNetworkRequest(QUrl(baseUrl_ + ".aff")));
        dicReply_->setParent(this);
        affReply_->setParent(this);

        connect(dicReply_, &QNetworkReply::finished, this, &AffDicDownloader::processDownloadedFile);
        connect(affReply_, &QNetworkReply::finished, this, &AffDicDownloader::processDownloadedFile);
        connect(dicReply_, &QNetworkReply::readyRead, this, &AffDicDownloader::handleReadyRead);
        connect(affReply_, &QNetworkReply::readyRead, this, &AffDicDownloader::handleReadyRead);
    }

    QString dicPath() const { return dicFile_.fileName(); }
    QString affPath() const { return affFile_.fileName(); }
    QString lastError() const { return lastError_; }
    bool    hasErrors() const { return !lastError_.isEmpty(); }

signals:
    void finished();

private slots:
    void handleReadyRead()
    {
        auto  reply = static_cast<QNetworkReply *>(sender());
        auto &file  = sender() == dicReply_ ? dicFile_ : affFile_;
        if (!file.write(reply->readAll())) {
            lastError_ = QLatin1String("Failed to write %1: %2").arg(file.fileName(), file.errorString());
            qDebug() << lastError_;
            reply->abort(); // will mark reply as failed and call finished()
        }
    }

    void processDownloadedFile()
    {
        auto reply = static_cast<QNetworkReply *>(sender());

        if (reply->error() != QNetworkReply::NoError) {
            if (lastError_.isEmpty()) {
                lastError_ = reply->errorString();
                qDebug() << lastError_;
            }
            auto siblingReply = reply == dicReply_ ? affReply_ : dicReply_;
            disconnect(siblingReply);
            if (!siblingReply->isFinished()) {
                siblingReply->abort();
            }
        } else {
            // qDebug() << "succefully downloaded" << reply->url();
        }
        if (dicReply_->isFinished() && affReply_->isFinished()) {
            if (!hasErrors()) {
                // qDebug() << "all files downloaded";
                if (!dicFile_.commit()) {
                    lastError_ = QLatin1String("Failed to commit .dic: ") + dicFile_.errorString();
                    qDebug() << lastError_;
                } else if (!affFile_.commit()) {
                    lastError_ = QLatin1String("Failed to commit .aff: ") + affFile_.errorString();
                    qDebug() << lastError_;
                    QFile::remove(dicFile_.fileName());
                }
                // qDebug() << (lastError_.isEmpty() ? QString("no errors") : lastError_);
            }
            emit finished();
            deleteLater();
        }
    }
};

HunspellDownloader::HunspellDownloader(const QLocale &locale, const QString &savePath, QNetworkAccessManager *nam,
                                       QObject *parent) :
    DictionaryDownloader(locale, parent), savePath_(savePath), nam_(nam)
{
    QDir().mkpath(savePath_);
}

void HunspellDownloader::start()
{
    static QPointer<DictListFetcher> dirFetcher;

    bool firstFetcher = !dirFetcher;
    if (!dirFetcher) {
        dirFetcher = new DictListFetcher(savePath_, nam_, this);
    }
    connect(dirFetcher, &DictListFetcher::finished, this, [this](const QStringList &dirList) {
        emit progress(10);
        cachedDirs_ = dirList;
        findAndDownloadDictionary();
    });
    if (firstFetcher) {
        dirFetcher->start();
    }
}

QString HunspellDownloader::findBestMatchingDirectory() const
{
    QString language = locale_.name().split(QLatin1Char('_'))[0].toLower();
    QString country
        = locale_.name().contains(QLatin1Char('_')) ? locale_.name().split(QLatin1Char('_'))[1].toUpper() : QString();

    if (!country.isEmpty()) {
        QString exactMatch = QStringLiteral("%1_%2").arg(language, country);
        if (cachedDirs_.contains(exactMatch)) {
            return exactMatch;
        }
    }

    if (cachedDirs_.contains(language)) {
        return language;
    }

    for (const QString &dir : cachedDirs_) {
        if (dir.startsWith(language + QLatin1Char('_'))) {
            return dir;
        }
    }

    return {};
}

void HunspellDownloader::findAndDownloadDictionary()
{
    QString bestMatch = findBestMatchingDirectory();
    if (bestMatch.isEmpty()) {
        lastError_ = QLatin1String("No matching dictionary found");
        emit finished();
        return;
    }
    auto baseUrl
        = QLatin1String("https://cgit.freedesktop.org/libreoffice/dictionaries/plain/") + bestMatch + QLatin1Char('/');
    auto        code  = locale_.name();
    QStringList codes = { { code } };
    if (code.contains(QLatin1Char('_'))) {
        codes << code.split('_')[0];
    }

    auto downloaders = std::make_shared<QList<AffDicDownloader *>>();
    for (auto const &baseName : std::as_const(codes)) {
        QString extLess    = baseUrl + baseName;
        auto    downloader = new AffDicDownloader(savePath_, extLess, nam_, this);
        downloaders->append(downloader);
        connect(downloader, &AffDicDownloader::finished, this, [this, downloader, downloaders]() {
            if (downloader->hasErrors()) {
                downloaders->removeOne(downloader); // will be deleted automatically later
                if (downloaders->empty()) {         // all failed
                    lastError_ += downloader->lastError();
                    lastError_ += '\n';
                }
            } else {
                for (auto d : std::as_const(*downloaders)) {
                    if (d != downloader) {
                        delete d; // d is in signal handler. safe to delete
                    }
                }
                downloaders->clear();
                dicPath_ = downloader->dicPath();
                affPath_ = downloader->affPath();
            }
            if (downloaders->empty()) {
                emit progress(100);
                emit finished();
                if (autoDelete_) {
                    deleteLater();
                }
            }
        });
    }
    for (auto d : std::as_const(*downloaders)) {
        d->start();
    }

    emit progress(50);
}

} // namespace AnyKeep

#include "hunspelldownloader.moc"
