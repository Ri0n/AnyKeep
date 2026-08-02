#include "yandexapiutils.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUuid>

namespace QtNote::YandexApi {
namespace {

    QString translate(const char *source) { return QCoreApplication::translate("QtNote::YandexApi", source); }

    QString responseMessage(const QByteArray &body)
    {
        const auto document = QJsonDocument::fromJson(body);
        if (!document.isObject())
            return {};

        const auto    object        = document.object();
        const auto    nested        = object.value(QLatin1String("error")).toObject();
        const QString nestedMessage = nested.value(QLatin1String("message")).toString().trimmed();
        if (!nestedMessage.isEmpty())
            return nestedMessage;
        return object.value(QLatin1String("message")).toString().trimmed();
    }

} // namespace

QNetworkRequest authenticatedRequest(const QUrl &url, const QString &apiKey)
{
    QNetworkRequest request(url);
    // SpeechKit responses are small, transient JSON documents. Qt 6.10 can
    // deliver HTTP/2 response data before its metadata and then try to enable
    // caching after bytes have already arrived. Neither HTTP/2 nor caching is
    // useful for these requests.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setRawHeader("Cache-Control", "no-store");
    request.setRawHeader("Authorization", QByteArray("Api-Key ") + apiKey.trimmed().toUtf8());
    request.setRawHeader("x-data-logging-enabled", "false");
    request.setRawHeader("x-client-request-id", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
    return request;
}

QString errorMessage(QNetworkReply *reply, const QByteArray &body)
{
    const int     status  = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
    const QString message = responseMessage(body);
    const QString folded  = message.toCaseFolded();

    if (status == 401 || folded.contains(QLatin1String("unauthenticated"))
        || folded.contains(QLatin1String("invalid api key"))) {
        return translate("The Yandex API key is invalid or has expired. Create a new key in Yandex AI Studio.");
    }

    if (status == 403) {
        if (folded.contains(QLatin1String("resource-manager.folder"))
            || folded.contains(QLatin1String("resource-manager.cloud"))
            || folded.contains(QLatin1String("organization-manager.organization"))) {
            return translate("Yandex Cloud denied access to the project. Check that the billing account is active "
                             "and that the API key has SpeechKit access.");
        }
        return translate("This Yandex API key is not allowed to use SpeechKit. Check the key permissions and the "
                         "Yandex Cloud billing status.");
    }

    if (!message.isEmpty())
        return message;
    const QString networkMessage = reply ? reply->errorString().trimmed() : QString();
    return networkMessage.isEmpty() ? translate("Yandex SpeechKit request failed") : networkMessage;
}

} // namespace QtNote::YandexApi
