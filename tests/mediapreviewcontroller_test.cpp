#include "draftmanager.h"
#include "localmediastore.h"
#include "mediaplaybackcontroller.h"
#include "mediapreviewcontroller.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "secureenvelope.h"

#include "editortestsupport.h"

#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

class MediaPreviewControllerTest : public QObject {
    Q_OBJECT
private slots:
    void extractsPosterWithoutChangingPlaybackOrDocument()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore store(directory.path(), SecureEnvelope::generateMasterKey());
        const auto      imported = store.importFile(QFINDTESTDATA("data/video-poster.avi"));
        QVERIFY2(imported, qPrintable(imported.error));
        const auto reference = imported.value;
        QVERIFY(reference.mediaType.startsWith(QLatin1String("video/")));
        Note note = plainNote();
        note.setText(QStringLiteral("Body"), Note::Markdown);
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor   editor(note, drafts);
        editor.setMedia({ reference });
        editor.model()->insertMedia(2, reference.uri(), QStringLiteral("Clip"), reference.mediaType);
        MediaPreviewController previews(&editor, nullptr, &store);
        QSignalSpy             changed(&previews, &MediaPreviewController::changed);
        const auto             text     = editor.text();
        const auto             manifest = editor.media();
        const bool             dirty    = editor.isDirty();
        const auto             undo     = editor.undoText();

        previews.requestPreview(reference.uri());
        QTRY_VERIFY_WITH_TIMEOUT(!previews.posterForUri(reference.uri()).isEmpty(), 8000);
        const auto   poster = previews.posterForUri(reference.uri()).toString();
        const auto   bytes  = QByteArray::fromBase64(poster.mid(poster.indexOf(QLatin1Char(',')) + 1).toLatin1());
        const QImage frame  = QImage::fromData(bytes);
        QVERIFY(!frame.isNull());
        QVERIFY(frame.width() <= 640 && frame.height() <= 480);
        const auto pixel = frame.pixelColor(frame.width() / 2, frame.height() / 2);
        QVERIFY(pixel.blue() > pixel.red() + 50);
        QCOMPARE(editor.text(), text);
        QCOMPARE(editor.media(), manifest);
        QCOMPARE(editor.isDirty(), dirty);
        QCOMPARE(editor.undoText(), undo);
        auto *playback = qobject_cast<MediaPlaybackController *>(editor.mediaPlayback());
        QVERIFY(playback);
        QVERIFY(playback->currentSourceUri().isEmpty());
        QVERIFY(!playback->playing());

        const int changes = changed.count();
        previews.requestPreview(reference.uri());
        QTest::qWait(50);
        QCOMPARE(changed.count(), changes);
        QCOMPARE(previews.posterForUri(reference.uri()).toString(), poster);

        editor.model()->removeBlock(2);
        QVERIFY(previews.posterForUri(reference.uri()).isEmpty());
    }

    void removedVideoDoesNotProduceStalePoster()
    {
        QTemporaryDir   directory;
        LocalMediaStore store(directory.path(), SecureEnvelope::generateMasterKey());
        const auto      imported = store.importFile(QFINDTESTDATA("data/video-poster.avi"));
        QVERIFY2(imported, qPrintable(imported.error));
        Note note = plainNote();
        note.setText(QStringLiteral("Body"), Note::Markdown);
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor   editor(note, drafts);
        editor.setMedia({ imported.value });
        editor.model()->insertMedia(2, imported.value.uri(), QStringLiteral("Clip"), imported.value.mediaType);
        MediaPreviewController previews(&editor, nullptr, &store);
        QSignalSpy             changed(&previews, &MediaPreviewController::changed);
        previews.requestPreview(imported.value.uri());
        editor.model()->removeBlock(2);
        QTest::qWait(50);
        QVERIFY(previews.posterForUri(imported.value.uri()).isEmpty());
        QCOMPARE(changed.count(), 0);
    }
};

QTEST_MAIN(MediaPreviewControllerTest)
#include "mediapreviewcontroller_test.moc"
