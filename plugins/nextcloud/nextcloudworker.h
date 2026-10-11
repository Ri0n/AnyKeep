#ifndef NEXTCLOUDWORKER_H
#define NEXTCLOUDWORKER_H

#include "nextclouddto.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>

class QHttpMultiPart;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace AnyKeep {

class NextcloudWorker final : public QObject {
public:
    explicit NextcloudWorker(QObject *parent = nullptr);

    void setConfig(const NextcloudConfig &config);

    NextcloudStatusResult probe();
    NextcloudListResult   listNotes();
    NextcloudNoteResult   getNote(const QString &id);
    NextcloudNoteResult   createNote(const NextcloudRemoteNote &note);
    NextcloudNoteResult   updateNote(const NextcloudRemoteNote &note);
    NextcloudNoteResult   updateNoteCategory(const QString &id, const QString &category, const QString &etag);
    NextcloudStatusResult deleteNote(const QString &id);

    // Native, non-encrypted Notes attachments: compatible with other clients.
    NextcloudAttachmentResult uploadAttachment(const QString &noteId, const QString &fileName,
                                               const QByteArray &data);
    NextcloudAttachmentResult downloadAttachment(const QString &noteId, const QString &path);

private:
    enum class Method { Get, Post, Put, Delete };

    struct HttpResult {
        bool                          ok { false };
        int                           status { 0 };
        QByteArray                    body;
        QHash<QByteArray, QByteArray> headers;
        QString                       error;
    };

    QNetworkAccessManager *manager();
    QUrl                   apiUrl(const QString &relativePath) const;
    QNetworkRequest        makeRequest(const QUrl &url) const;
    HttpResult             collectReply(QNetworkReply *reply);
    HttpResult             performMultipart(const QUrl &url, QHttpMultiPart *multipart);
    HttpResult             perform(Method method, const QUrl &url, const QByteArray &body = {},
                                   const QHash<QByteArray, QByteArray> &extraHeaders = {});

    static QByteArray                         headerValue(const HttpResult &reply, const QByteArray &name);
    static QString                            errorText(const HttpResult &reply);
    static std::optional<NextcloudRemoteNote> parseNote(const QByteArray &json, QString *error);
    static std::optional<NextcloudRemoteNote> parseNoteObject(const QJsonObject &object, QString *error);
    static QByteArray                         serializeNote(const NextcloudRemoteNote &note);
    static QByteArray                         serializeCategory(const QString &category);

    NextcloudConfig        config_;
    QNetworkAccessManager *manager_ { nullptr };
};

} // namespace AnyKeep

#endif // NEXTCLOUDWORKER_H
