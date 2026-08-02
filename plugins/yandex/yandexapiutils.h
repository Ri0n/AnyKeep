#ifndef YANDEXAPIUTILS_H
#define YANDEXAPIUTILS_H

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

class QNetworkReply;

namespace QtNote::YandexApi {

QNetworkRequest authenticatedRequest(const QUrl &url, const QString &apiKey);
QString         errorMessage(QNetworkReply *reply, const QByteArray &body);

} // namespace QtNote::YandexApi

#endif // YANDEXAPIUTILS_H
