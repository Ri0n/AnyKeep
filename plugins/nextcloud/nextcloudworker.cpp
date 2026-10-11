#include "nextcloudworker.h"
#include "nextcloudmedialinks.h"

#include <QEventLoop>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <limits>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>

namespace AnyKeep {

namespace {

    QString normalizePath(QString path)
    {
        while (path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }

        const QString apiSuffix = QStringLiteral("/index.php/apps/notes/api/v1");
        const int     apiPos    = path.indexOf(apiSuffix);
        if (apiPos >= 0) {
            path.truncate(apiPos + apiSuffix.size());
            return path;
        }

        if (path.endsWith(QStringLiteral("/index.php"))) {
            path.chop(QStringLiteral("/index.php").size());
        }

        return path + apiSuffix;
    }

    // HTTP If-Match uses the quoted entity-tag syntax (RFC 9110). The
    // Nextcloud Notes JSON "etag" attribute itself contains only the tag.
    QByteArray ifMatchValue(const QString &etag)
    {
        const QByteArray value = etag.toUtf8();
        if (value.startsWith('"') && value.endsWith('"'))
            return value;
        return QByteArrayLiteral("\"") + value + QByteArrayLiteral("\"");
    }

    QString serverMessage(const QByteArray &body)
    {
        QJsonParseError parseError;
        const auto      document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            return {};
        }

        const auto object = document.object();
        for (const auto &key : { QStringLiteral("message"), QStringLiteral("error"), QStringLiteral("detail") }) {
            const auto value = object.value(key);
            if (value.isString() && !value.toString().isEmpty()) {
                return value.toString();
            }
        }
        return {};
    }

} // namespace

NextcloudWorker::NextcloudWorker(QObject *parent) : QObject(parent) {}

void NextcloudWorker::setConfig(const NextcloudConfig &config)
{
    config_ = config;
    if (manager_) {
        manager_->clearAccessCache();
        manager_->clearConnectionCache();
    }
}

QNetworkAccessManager *NextcloudWorker::manager()
{
    if (!manager_) {
        manager_ = new QNetworkAccessManager(this);
    }
    return manager_;
}

QUrl NextcloudWorker::apiUrl(const QString &relativePath) const
{
    QUrl url = config_.serverUrl;
    url.setQuery(QString());
    url.setFragment(QString());

    QString path = normalizePath(url.path());
    if (!relativePath.isEmpty()) {
        if (!relativePath.startsWith(QLatin1Char('/'))) {
            path += QLatin1Char('/');
        }
        path += relativePath;
    }
    url.setPath(path);
    return url;
}

QNetworkRequest NextcloudWorker::makeRequest(const QUrl &url) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "AnyKeep Nextcloud Notes/1.0");

    const QByteArray credentials = (config_.userName + QLatin1Char(':') + config_.appPassword).toUtf8().toBase64();
    request.setRawHeader("Authorization", "Basic " + credentials);

    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

NextcloudWorker::HttpResult NextcloudWorker::collectReply(QNetworkReply *reply)
{
    HttpResult result;
    QEventLoop loop;
    QTimer     timeout;
    timeout.setSingleShot(true);

    bool timedOut = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, reply, [&]() {
        timedOut = true;
        reply->abort();
    });

    timeout.start(qMax(1000, config_.timeoutMs));
    loop.exec();
    timeout.stop();

    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body   = reply->readAll();

    for (const auto &pair : reply->rawHeaderPairs()) {
        result.headers.insert(pair.first, pair.second);
    }

    result.ok = !timedOut && reply->error() == QNetworkReply::NoError && result.status >= 200 && result.status < 300;

    if (timedOut) {
        result.error = QStringLiteral("Request timed out after %1 ms").arg(config_.timeoutMs);
    } else if (!result.ok) {
        result.error       = reply->errorString();
        const auto message = serverMessage(result.body);
        if (!message.isEmpty()) {
            result.error += QStringLiteral(": ") + message;
        }
    }

    reply->deleteLater();
    return result;
}

NextcloudWorker::HttpResult NextcloudWorker::performMultipart(const QUrl &url, QHttpMultiPart *multipart)
{
    auto request = makeRequest(url);
    QNetworkReply *reply = manager()->post(request, multipart);
    multipart->setParent(reply);
    return collectReply(reply);
}

NextcloudWorker::HttpResult NextcloudWorker::perform(Method method, const QUrl &url, const QByteArray &body,
                                                     const QHash<QByteArray, QByteArray> &extraHeaders)
{
    HttpResult result;
    auto       request = makeRequest(url);

    for (auto it = extraHeaders.cbegin(); it != extraHeaders.cend(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }

    QNetworkReply *reply = nullptr;
    switch (method) {
    case Method::Get:
        reply = manager()->get(request);
        break;
    case Method::Post:
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = manager()->post(request, body);
        break;
    case Method::Put:
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = manager()->put(request, body);
        break;
    case Method::Delete:
        reply = manager()->deleteResource(request);
        break;
    }

    return collectReply(reply);
}

QByteArray NextcloudWorker::headerValue(const HttpResult &reply, const QByteArray &name)
{
    for (auto it = reply.headers.cbegin(); it != reply.headers.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) {
            return it.value();
        }
    }
    return {};
}

QString NextcloudWorker::errorText(const HttpResult &reply)
{
    QString prefix;
    switch (reply.status) {
    case 0:
        prefix = QStringLiteral("Network error");
        break;
    case 401:
        prefix = QStringLiteral("Authentication failed");
        break;
    case 403:
        prefix = QStringLiteral("Operation is forbidden; the note may be read-only");
        break;
    case 404:
        prefix = QStringLiteral("Notes endpoint or note was not found");
        break;
    case 412:
        prefix = QStringLiteral("The note was changed on the server");
        break;
    case 507:
        prefix = QStringLiteral("Nextcloud reports insufficient storage");
        break;
    default:
        prefix = QStringLiteral("HTTP error %1").arg(reply.status);
        break;
    }

    if (!reply.error.isEmpty()) {
        prefix += QStringLiteral(": ") + reply.error;
    }
    return prefix;
}

std::optional<NextcloudRemoteNote> NextcloudWorker::parseNoteObject(const QJsonObject &object, QString *error)
{
    const auto idValue = object.value(QStringLiteral("id"));
    if (!idValue.isDouble() && !idValue.isString()) {
        if (error) {
            *error = QStringLiteral("The server returned a note without a valid id");
        }
        return std::nullopt;
    }

    NextcloudRemoteNote note;
    note.id       = idValue.isString() ? idValue.toString() : QString::number(idValue.toVariant().toLongLong());
    note.etag     = object.value(QStringLiteral("etag")).toString();
    note.readOnly = object.value(QStringLiteral("readonly")).toBool(false);

    note.contentPresent = object.contains(QStringLiteral("content"));
    if (note.contentPresent) {
        note.content = object.value(QStringLiteral("content")).toString();
    }

    note.title    = object.value(QStringLiteral("title")).toString();
    note.category = object.value(QStringLiteral("category")).toString();
    note.favorite = object.value(QStringLiteral("favorite")).toBool(false);
    note.modified = object.value(QStringLiteral("modified")).toVariant().toLongLong();

    return note;
}

std::optional<NextcloudRemoteNote> NextcloudWorker::parseNote(const QByteArray &json, QString *error)
{
    QJsonParseError parseError;
    const auto      document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Invalid JSON note response: %1").arg(parseError.errorString());
        }
        return std::nullopt;
    }
    return parseNoteObject(document.object(), error);
}

QByteArray NextcloudWorker::serializeNote(const NextcloudRemoteNote &note)
{
    QJsonObject object;
    object.insert(QStringLiteral("title"), note.title);
    object.insert(QStringLiteral("content"), note.content);
    object.insert(QStringLiteral("category"), note.category);
    object.insert(QStringLiteral("favorite"), note.favorite);
    object.insert(QStringLiteral("modified"), static_cast<double>(note.modified));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray NextcloudWorker::serializeCategory(const QString &category)
{
    QJsonObject object;
    object.insert(QStringLiteral("category"), category);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

NextcloudStatusResult NextcloudWorker::probe()
{
    QUrl      url = apiUrl(QStringLiteral("notes"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("exclude"),
                       QStringLiteral("content,title,category,favorite,modified,etag,readonly"));
    query.addQueryItem(QStringLiteral("chunkSize"), QStringLiteral("1"));
    url.setQuery(query);

    const auto reply = perform(Method::Get, url);

    NextcloudStatusResult result;
    result.ok         = reply.ok;
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QJsonParseError parseError;
    const auto      document = QJsonDocument::fromJson(reply.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        result.ok    = false;
        result.error = QStringLiteral("The URL responded, but it is not a Nextcloud Notes API endpoint");
    }
    return result;
}

NextcloudListResult NextcloudWorker::listNotes()
{
    NextcloudListResult result;
    QString             cursor;
    QSet<QString>       seenCursors;

    for (;;) {
        QUrl      url = apiUrl(QStringLiteral("notes"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("exclude"), QStringLiteral("content"));
        query.addQueryItem(QStringLiteral("chunkSize"), QStringLiteral("100"));
        if (!cursor.isEmpty()) {
            query.addQueryItem(QStringLiteral("chunkCursor"), cursor);
        }
        url.setQuery(query);

        const auto reply  = perform(Method::Get, url);
        result.httpStatus = reply.status;
        if (!reply.ok) {
            result.error = errorText(reply);
            return result;
        }

        QJsonParseError parseError;
        const auto      document = QJsonDocument::fromJson(reply.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
            result.error = QStringLiteral("Invalid notes list JSON: %1").arg(parseError.errorString());
            return result;
        }

        for (const auto &value : document.array()) {
            if (!value.isObject()) {
                result.error = QStringLiteral("The notes list contains a non-object item");
                return result;
            }

            QString parseNoteError;
            auto    note = parseNoteObject(value.toObject(), &parseNoteError);
            if (!note) {
                result.error = parseNoteError;
                return result;
            }
            result.notes.append(*note);
        }

        const QString nextCursor = QString::fromUtf8(headerValue(reply, QByteArrayLiteral("X-Notes-Chunk-Cursor")));
        if (nextCursor.isEmpty()) {
            result.ok = true;
            return result;
        }
        if (seenCursors.contains(nextCursor)) {
            result.error = QStringLiteral("The server returned a repeated pagination cursor");
            return result;
        }

        seenCursors.insert(nextCursor);
        cursor = nextCursor;
    }
}

NextcloudNoteResult NextcloudWorker::getNote(const QString &id)
{
    const auto reply = perform(Method::Get, apiUrl(QStringLiteral("notes/") + id));

    NextcloudNoteResult result;
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QString parseError;
    auto    note = parseNote(reply.body, &parseError);
    if (!note) {
        result.error = parseError;
        return result;
    }

    result.note = *note;
    result.ok   = true;
    return result;
}

NextcloudNoteResult NextcloudWorker::createNote(const NextcloudRemoteNote &note)
{
    const auto reply = perform(Method::Post, apiUrl(QStringLiteral("notes")), serializeNote(note));

    NextcloudNoteResult result;
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QString parseError;
    auto    created = parseNote(reply.body, &parseError);
    if (!created) {
        result.error = parseError;
        return result;
    }

    result.note = *created;
    result.ok   = true;
    return result;
}

NextcloudNoteResult NextcloudWorker::updateNote(const NextcloudRemoteNote &note)
{
    QHash<QByteArray, QByteArray> headers;
    if (!note.etag.isEmpty()) {
        headers.insert(QByteArrayLiteral("If-Match"), ifMatchValue(note.etag));
    }

    const auto reply = perform(Method::Put, apiUrl(QStringLiteral("notes/") + note.id), serializeNote(note), headers);

    NextcloudNoteResult result;
    result.httpStatus = reply.status;

    if (reply.status == 412) {
        result.conflict = true;
        QString parseError;
        result.remoteOnConflict = parseNote(reply.body, &parseError);
        result.error            = errorText(reply);
        if (!parseError.isEmpty()) {
            result.error += QStringLiteral("; ") + parseError;
        }
        return result;
    }

    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QString parseError;
    auto    updated = parseNote(reply.body, &parseError);
    if (!updated) {
        result.error = parseError;
        return result;
    }

    result.note = *updated;
    result.ok   = true;
    return result;
}

NextcloudNoteResult NextcloudWorker::updateNoteCategory(const QString &id, const QString &category, const QString &etag)
{
    QHash<QByteArray, QByteArray> headers;
    if (!etag.isEmpty())
        headers.insert(QByteArrayLiteral("If-Match"), ifMatchValue(etag));

    const auto reply
        = perform(Method::Put, apiUrl(QStringLiteral("notes/") + id), serializeCategory(category), headers);

    NextcloudNoteResult result;
    result.httpStatus = reply.status;
    if (reply.status == 412) {
        result.conflict = true;
        QString parseError;
        result.remoteOnConflict = parseNote(reply.body, &parseError);
        result.error            = errorText(reply);
        if (!parseError.isEmpty())
            result.error += QStringLiteral("; ") + parseError;
        return result;
    }
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QString parseError;
    auto    updated = parseNote(reply.body, &parseError);
    if (!updated) {
        result.error = parseError;
        return result;
    }
    result.note = *updated;
    result.ok   = true;
    return result;
}

NextcloudStatusResult NextcloudWorker::deleteNote(const QString &id)
{
    const auto reply = perform(Method::Delete, apiUrl(QStringLiteral("notes/") + id));

    NextcloudStatusResult result;
    result.ok         = reply.ok;
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
    }
    return result;
}

NextcloudAttachmentResult NextcloudWorker::uploadAttachment(const QString &noteId, const QString &fileName,
                                                            const QByteArray &data)
{
    NextcloudAttachmentResult result;
    // Disallow remote path traversal and multipart header injection.
    if (noteId.isEmpty() || fileName.isEmpty() || fileName == QStringLiteral(".")
        || fileName == QStringLiteral("..") || fileName.contains(QLatin1Char('/'))
        || fileName.contains(QLatin1Char('\\')) || fileName.contains(QLatin1Char('"'))
        || fileName.contains(QLatin1Char('\r')) || fileName.contains(QLatin1Char('\n'))) {
        result.error = QStringLiteral("Invalid attachment file name or note ID");
        return result;
    }

    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    const QByteArray disposition = QByteArrayLiteral("form-data; name=\"file\"; filename=\"")
        + fileName.toUtf8() + QByteArrayLiteral("\"");
    part.setHeader(QNetworkRequest::ContentDispositionHeader, disposition);
    part.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    part.setBody(data);
    multipart->append(part);

    // Nextcloud Notes 5.x exposes attachments on the v1.4 route, while the
    // ordinary notes CRUD endpoints stay on v1.
    QUrl url = apiUrl(QStringLiteral("attachment/") + noteId);
    QString path = url.path();
    path.replace(QStringLiteral("/api/v1/"), QStringLiteral("/api/v1.4/"));
    url.setPath(path);

    const auto reply = performMultipart(url, multipart);
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(reply.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = QStringLiteral("Invalid attachment upload response");
        return result;
    }
    result.path = document.object().value(QStringLiteral("filename")).toString();
    if (result.path.isEmpty() || result.path.startsWith(QLatin1Char('/'))
        || result.path.contains(QStringLiteral("..")) || result.path.contains(QLatin1Char('\\'))) {
        result.error = QStringLiteral("Nextcloud returned an unsafe attachment path");
        result.path.clear();
        return result;
    }
    result.ok = true;
    return result;
}

NextcloudAttachmentResult NextcloudWorker::downloadAttachment(const QString &noteId, const QString &path)
{
    NextcloudAttachmentResult result;
    if (noteId.isEmpty() || path.isEmpty() || path.startsWith(QLatin1Char('/'))
        || path.contains(QStringLiteral("..")) || path.contains(QLatin1Char('\\'))) {
        result.error = QStringLiteral("Invalid attachment path");
        return result;
    }

    QUrl url = apiUrl(QStringLiteral("attachment/") + noteId);
    QString endpoint = url.path();
    endpoint.replace(QStringLiteral("/api/v1/"), QStringLiteral("/api/v1.4/"));
    url.setPath(endpoint);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("path"), path);
    url.setQuery(query);
    const auto reply = perform(Method::Get, url, {},
                               { { QByteArrayLiteral("Accept"), QByteArrayLiteral("application/octet-stream") } });
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }
    result.path = path;
    result.data = reply.body;
    result.ok = true;
    return result;
}

NextcloudSettingsResult NextcloudWorker::getSettings()
{
    const auto reply = perform(Method::Get, apiUrl(QStringLiteral("settings")));
    NextcloudSettingsResult result;
    result.httpStatus = reply.status;
    if (!reply.ok) {
        result.error = errorText(reply);
        return result;
    }

    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(reply.body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        result.error = QStringLiteral("Invalid Nextcloud Notes settings response");
        return result;
    }
    result.notesPath = json.object().value(QStringLiteral("notesPath")).toString();
    if (!NextcloudMediaLinks::isSafeNativePath(result.notesPath)) {
        result.error = QStringLiteral("Notes folder path is missing or invalid");
        return result;
    }
    result.ok = true;
    return result;
}

NextcloudWebDavRangeResult NextcloudWorker::readWebDavRange(const QString &relativeFilePath, qint64 offset, qint64 count)
{
    NextcloudWebDavRangeResult result;
    // Ranged reads must never accidentally pull entire large remote files.
    if (!NextcloudMediaLinks::isSafeNativePath(relativeFilePath) || offset < 0
        || count <= 0 || count > 1024 * 1024 || offset > std::numeric_limits<qint64>::max() - count) {
        result.error = QStringLiteral("Invalid WebDAV range or remote path");
        return result;
    }

    QUrl url = config_.serverUrl;
    url.setQuery(QString());
    url.setFragment(QString());
    QString base = url.path();
    const auto indexPhp = base.indexOf(QStringLiteral("/index.php"));
    if (indexPhp >= 0)
        base.truncate(indexPhp);
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    url.setPath(base + QStringLiteral("/remote.php/dav/files/") + config_.userName
                + QLatin1Char('/') + relativeFilePath);

    const QByteArray range = QByteArrayLiteral("bytes=") + QByteArray::number(offset) + '-'
        + QByteArray::number(offset + count - 1);
    const auto reply = perform(Method::Get, url, {},
                               { { QByteArrayLiteral("Range"), range },
                                 { QByteArrayLiteral("Accept"), QByteArrayLiteral("application/octet-stream") } });
    result.httpStatus = reply.status;
    if (!reply.ok || reply.status != 206) {
        result.error = reply.status == 200
            ? QStringLiteral("WebDAV server ignored the byte-range request")
            : errorText(reply);
        return result;
    }

    static const QRegularExpression contentRangePattern(
        QStringLiteral(R"(^bytes (\d+)-(\d+)/(\d+)$)"));
    const QString header = QString::fromLatin1(headerValue(reply, QByteArrayLiteral("Content-Range")));
    const auto match = contentRangePattern.match(header);
    if (!match.hasMatch()) {
        result.error = QStringLiteral("WebDAV response is missing a valid Content-Range");
        return result;
    }
    bool fromOk = false;
    bool toOk = false;
    bool totalOk = false;
    const qint64 from = match.captured(1).toLongLong(&fromOk);
    const qint64 to = match.captured(2).toLongLong(&toOk);
    const qint64 total = match.captured(3).toLongLong(&totalOk);
    if (!fromOk || !toOk || !totalOk || from != offset || to < from
        || to - from + 1 != reply.body.size() || reply.body.size() > count || total <= to) {
        result.error = QStringLiteral("WebDAV returned an inconsistent byte range");
        return result;
    }
    result.data = reply.body;
    result.totalSize = total;
    result.ok = true;
    return result;
}

} // namespace AnyKeep
