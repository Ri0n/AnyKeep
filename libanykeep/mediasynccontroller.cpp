#include "mediasynccontroller.h"

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include <QSet>
#include <utility>

namespace AnyKeep {
MediaSyncController::MediaSyncController(NoteEditor *editor, QObject *parent) :
    QObject(parent), editor_(editor)
{
    connect(editor, &NoteEditor::mediaChanged, this, [this] { refresh(); });
    connect(editor->model(), &NoteBlockModel::contentsChanged, this, [this] { refresh(); });
    connect(MediaSyncService::instance(), &MediaSyncService::snapshotChanged, this,
            [this](const QString &id) {
                for (const auto &item : std::as_const(media_)) {
                    if (MediaSyncService::key(item) == id) {
                        notifyChange();
                        break;
                    }
                }
            });
    refresh();
}

MediaSyncController::~MediaSyncController()
{
    if (auto *service = MediaSyncService::instance())
        service->unwatch(this);
}

void MediaSyncController::notifyChange()
{
    ++revision_;
    emit changed();
}

void MediaSyncController::refresh()
{
    QSet<QString> usedUris;
    const auto *model = editor_->model();
    for (int row = 0; model && row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        const auto type = model->data(index, NoteBlockModel::TypeRole).toInt();
        if (type == NoteBlockModel::Media || type == NoteBlockModel::Attachment)
            usedUris.insert(model->data(index, NoteBlockModel::UrlRole).toString());
    }

    QList<MediaReference> nextMedia;
    QHash<QString, MediaReference> nextByUri;
    QSet<QString> seen;
    for (const auto &reference : editor_->media()) {
        if (!usedUris.contains(reference.uri()))
            continue;
        nextByUri.insert(reference.uri(), reference);
        const QString id = MediaSyncService::key(reference);
        if (seen.contains(id))
            continue;
        seen.insert(id);
        nextMedia.append(reference);
    }
    // Ordinary text edits must not restart background media observers or
    // their Jingle sessions. Rebind only when the referenced media changes.
    if (nextMedia == media_ && nextByUri == byUri_)
        return;

    auto *service = MediaSyncService::instance();
    service->unwatch(this);
    media_ = std::move(nextMedia);
    byUri_ = std::move(nextByUri);
    for (const auto &reference : std::as_const(media_))
        if (!LocalMediaStore::instance()->contains(reference))
            service->watch(this, reference);
    notifyChange();
}

MediaSyncSnapshot MediaSyncController::status(const MediaReference &reference) const
{
    MediaSyncSnapshot result;
    result.totalBytes = qMax<qint64>(0, reference.size);
    if (LocalMediaStore::instance()->contains(reference)) {
        result.verifiedBytes = result.totalBytes;
        result.state = MediaSyncSnapshot::Complete;
        return result;
    }
    return MediaSyncService::instance()->snapshot(reference);
}

double MediaSyncController::progress() const
{
    // Byte-weighted, never an average of percentages. Unique immutable
    // content contributes once even if used by multiple document blocks.
    long double available = 0, total = 0;
    for (const auto &reference : media_) {
        const auto snapshot = status(reference);
        total += snapshot.totalBytes;
        available += snapshot.verifiedBytes;
    }
    return total > 0 ? double(qBound((long double)0, available / total, (long double)1))
                     : (media_.isEmpty() || state() != MediaSyncSnapshot::Complete ? 0.0 : 1.0);
}

int MediaSyncController::state() const
{
    if (media_.isEmpty())
        return MediaSyncSnapshot::Complete;
    bool downloading = false;
    bool failed = false;
    bool allComplete = true;
    for (const auto &reference : media_) {
        const auto snapshot = status(reference);
        if (snapshot.state != MediaSyncSnapshot::Complete)
            allComplete = false;
        if (snapshot.state == MediaSyncSnapshot::Transferring)
            downloading = true;
        if (snapshot.state == MediaSyncSnapshot::Failed)
            failed = true;
    }
    if (allComplete)
        return MediaSyncSnapshot::Complete;
    if (failed)
        return MediaSyncSnapshot::Failed;
    if (downloading)
        return MediaSyncSnapshot::Transferring;
    return MediaSyncSnapshot::Waiting;
}

QVariantMap MediaSyncController::statusForUri(const QString &uri) const
{
    const auto it = byUri_.constFind(uri);
    if (it == byUri_.cend())
        return { { QStringLiteral("valid"), false } };
    const auto snapshot = status(*it);
    const double fraction = snapshot.totalBytes > 0
        ? double(snapshot.verifiedBytes) / double(snapshot.totalBytes)
        : (snapshot.state == MediaSyncSnapshot::Complete ? 1.0 : 0.0);
    return { { QStringLiteral("valid"), true },
             { QStringLiteral("progress"), fraction },
             { QStringLiteral("state"), int(snapshot.state) },
             { QStringLiteral("error"), snapshot.error } };
}
} // namespace AnyKeep
