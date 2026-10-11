#ifndef NEXTCLOUDDTO_H
#define NEXTCLOUDDTO_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

namespace AnyKeep {

struct NextcloudConfig {
    QUrl    serverUrl;
    QString userName;
    QString appPassword;
    int     timeoutMs { 15000 };
};

struct NextcloudRemoteNote {
    QString id;
    QString etag;
    bool    readOnly { false };

    QString content;
    bool    contentPresent { false };

    QString title;
    QString category;
    bool    favorite { false };
    qint64  modified { 0 };
};

struct NextcloudStatusResult {
    bool    ok { false };
    int     httpStatus { 0 };
    QString error;
};

struct NextcloudSettingsResult : NextcloudStatusResult {
    QString notesPath;
};

struct NextcloudWebDavUploadResult : NextcloudStatusResult {
    QString path;
};

struct NextcloudWebDavRangeResult : NextcloudStatusResult {
    QByteArray data;
    qint64 totalSize { -1 };
    // Opaque WebDAV ETag, including quotes. A multi-range reader can use
    // If-Match to ensure a seek never joins different file revisions.
    QString etag;
};

struct NextcloudAttachmentResult : NextcloudStatusResult {
    QString path;
    QByteArray data;
};

struct NextcloudListResult : NextcloudStatusResult {
    QList<NextcloudRemoteNote> notes;
};

struct NextcloudNoteResult : NextcloudStatusResult {
    NextcloudRemoteNote                note;
    bool                               conflict { false };
    std::optional<NextcloudRemoteNote> remoteOnConflict;
};

} // namespace AnyKeep

#endif // NEXTCLOUDDTO_H
