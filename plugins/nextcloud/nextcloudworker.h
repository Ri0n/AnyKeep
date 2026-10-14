#ifndef NEXTCLOUDWORKER_H
#define NEXTCLOUDWORKER_H

#include "nextclouddto.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <functional>

class QHttpMultiPart;
class QIODevice;
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
    NextcloudSettingsResult getSettings();
    // Create a Notes-native per-note attachment directory; 405 means it already exists.
    NextcloudStatusResult       ensureWebDavFolder(const QString &relativeFolderPath);
    NextcloudWebDavUploadResult uploadWebDavChunks(const QString &relativeFilePath, QIODevice *source, qint64 size);
    NextcloudWebDavRangeResult readWebDavRange(const QString &relativeFilePath, qint64 offset, qint64 count,
                                                const QString &ifMatchEtag = {});
    // Non-blocking, pinned-account WebDAV media fetch. Completion runs on the
    // worker thread; callers must forward results to their own thread.
    void readWebDavRangeAsync(NextcloudConfig config, QString relativeFilePath, qint64 offset, qint64 count,
                              QString ifMatchEtag, std::function<void(NextcloudWebDavRangeResult)> done);

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
    QUrl                   webDavFileUrl(const QString &relativeFilePath) const;
    QUrl                   webDavFileUrl(const QString &relativeFilePath, const NextcloudConfig &config) const;
    QUrl                   webDavUploadUrl(const QString &uploadId) const;
    QNetworkRequest        makeRequest(const QUrl &url) const;
    QNetworkRequest        makeRequest(const QUrl &url, const NextcloudConfig &config) const;
    HttpResult             collectReply(QNetworkReply *reply, qint64 maxBodyBytes = 32 * 1024 * 1024);
    NextcloudWebDavRangeResult validateWebDavRange(const HttpResult &reply, qint64 offset, qint64 count,
                                                    const QString &ifMatchEtag) const;
    HttpResult             performMultipart(const QUrl &url, QHttpMultiPart *multipart);
    HttpResult             perform(Method method, const QUrl &url, const QByteArray &body = {},
                                   const QHash<QByteArray, QByteArray> &extraHeaders = {},
                                   qint64 maxBodyBytes = 32 * 1024 * 1024);

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
