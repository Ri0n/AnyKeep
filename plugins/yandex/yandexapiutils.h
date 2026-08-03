#ifndef YANDEXAPIUTILS_H
#define YANDEXAPIUTILS_H

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

class QNetworkReply;

namespace AnyKeep::YandexApi {

QNetworkRequest authenticatedRequest(const QUrl &url, const QString &apiKey);
QString         errorMessage(QNetworkReply *reply, const QByteArray &body);

} // namespace AnyKeep::YandexApi

#endif // YANDEXAPIUTILS_H
