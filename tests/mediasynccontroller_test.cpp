#include "draftmanager.h"
#include "mediareference.h"
#include "mediasynccontroller.h"
#include "mediasyncservice.h"
#include "notedata.h"
#include "noteeditor.h"

#include "editortestsupport.h"

#include <QCryptographicHash>
#include <QtTest>
#include <memory>

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

namespace {
MediaReference reference(qint64 size, const QByteArray &identity)
{
    MediaReference media;
    media.id = QUuid::createUuid();
    media.portableName = QStringLiteral("asset.png");
    media.originalName = media.portableName;
    media.mediaType = QStringLiteral("image/png");
    media.size = size;
    media.checksum = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
    media.remoteData.insert(QStringLiteral("xmpp.instance"), QStringLiteral("media-sync-test"));
    return media;
}

Note noteWithMedia(const QList<MediaReference> &media, const QList<int> &references)
{
    Note note(new NoteData(nullptr));
    note.setTitle(QStringLiteral("Progress"));
    QStringList lines;
    for (int row : references)
        lines.append(QStringLiteral("![image](%1)").arg(media.at(row).uri()));
    note.setText(lines.join(QStringLiteral("\n\n")), Note::Markdown);
    note.setMedia(media);
    return note;
}

MediaSyncSnapshot snapshot(qint64 bytes, qint64 size, MediaSyncSnapshot::State state)
{
    MediaSyncSnapshot result;
    result.verifiedBytes = bytes;
    result.totalBytes = size;
    result.state = state;
    return result;
}
} // namespace

class MediaSyncControllerTest : public QObject {
    Q_OBJECT
private slots:
    void byteWeightedProgressAndUniqueReferences()
    {
        const auto first = reference(100, QUuid::createUuid().toRfc4122());
        const auto second = reference(900, QUuid::createUuid().toRfc4122());
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor editor(noteWithMedia({first, second}, {0, 1, 0, 1}), drafts);
        auto *controller = qobject_cast<MediaSyncController *>(editor.mediaSync());
        QVERIFY(controller);
        QVERIFY(controller->hasMedia());
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Waiting));
        QCOMPARE(controller->progress(), 0.0);

        auto *service = MediaSyncService::instance();
        service->publish(first, snapshot(100, 100, MediaSyncSnapshot::Complete));
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Waiting));
        QCOMPARE(controller->progress(), 0.1);
        service->publish(second, snapshot(450, 900, MediaSyncSnapshot::Transferring));
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Transferring));
        QCOMPARE(controller->progress(), 0.55);
        // Seeking/retrying the same already-saved chunk cannot increase bytes.
        service->publish(second, snapshot(450, 900, MediaSyncSnapshot::Waiting));
        QCOMPARE(controller->progress(), 0.55);
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Waiting));
        service->publish(second, snapshot(900, 900, MediaSyncSnapshot::Complete));
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Complete));
        QCOMPARE(controller->progress(), 1.0);
    }

    void deduplicatesSameContentWithDifferentUris()
    {
        const auto first = reference(100, QUuid::createUuid().toRfc4122());
        auto another = first;
        another.id = QUuid::createUuid();
        another.portableName = QStringLiteral("copy.png");
        const auto other = reference(900, QUuid::createUuid().toRfc4122());
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor editor(noteWithMedia({first, another, other}, {0, 1, 2}), drafts);
        auto *controller = qobject_cast<MediaSyncController *>(editor.mediaSync());
        QVERIFY(controller);

        MediaSyncService::instance()->publish(first, snapshot(100, 100, MediaSyncSnapshot::Complete));
        QCOMPARE(controller->progress(), 0.1); // not 200 / 1100
        QCOMPARE(controller->statusForUri(another.uri()).value(QStringLiteral("progress")).toDouble(), 1.0);
    }

    void failureWaitingAndUnreferencedMediaRemoval()
    {
        const auto first = reference(500, QUuid::createUuid().toRfc4122());
        const auto second = reference(500, QUuid::createUuid().toRfc4122());
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor editor(noteWithMedia({first, second}, {0, 1}), drafts);
        auto *controller = qobject_cast<MediaSyncController *>(editor.mediaSync());
        QVERIFY(controller);

        MediaSyncService::instance()->publish(first, snapshot(250, 500, MediaSyncSnapshot::Failed));
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Failed));
        QCOMPARE(controller->progress(), 0.25);
        // Deleting the failing block must unsubscribe it and reweight totals.
        editor.setText(QStringLiteral("![remaining](%1)").arg(second.uri()));
        QVERIFY(!controller->statusForUri(first.uri()).value(QStringLiteral("valid")).toBool());
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Waiting));
        QCOMPARE(controller->progress(), 0.0);
        editor.setText(QStringLiteral("No media"));
        QVERIFY(!controller->hasMedia());
        QCOMPARE(controller->state(), int(MediaSyncSnapshot::Complete));
    }

    void switchingEditorsDoesNotRetainOtherNotes()
    {
        const auto first = reference(300, QUuid::createUuid().toRfc4122());
        const auto second = reference(600, QUuid::createUuid().toRfc4122());
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        auto editorA = std::make_unique<NoteEditor>(noteWithMedia({first}, {0}), drafts);
        NoteEditor editorB(noteWithMedia({second}, {0}), drafts);
        auto *controllerB = qobject_cast<MediaSyncController *>(editorB.mediaSync());
        QVERIFY(controllerB);
        editorA.reset(); // destroys all first-note subscriptions
        QCOMPARE(controllerB->progress(), 0.0);
        MediaSyncService::instance()->publish(first, snapshot(300, 300, MediaSyncSnapshot::Complete));
        QCOMPARE(controllerB->progress(), 0.0);
        MediaSyncService::instance()->publish(second, snapshot(600, 600, MediaSyncSnapshot::Complete));
        QCOMPARE(controllerB->progress(), 1.0);
    }
};

QTEST_MAIN(MediaSyncControllerTest)
#include "mediasynccontroller_test.moc"
