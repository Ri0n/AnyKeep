#include "mediaplaybackcontroller.h"
#include "mediarangeservice.h"
#include "noteeditor.h"

#include <QDataStream>
#include <QtTest>
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#endif

using namespace AnyKeep;

class MediaPlaybackSuspendTest : public QObject {
    Q_OBJECT
    static QByteArray silentWav()
    {
        QByteArray  wav;
        QDataStream data(&wav, QIODevice::WriteOnly);
        data.setByteOrder(QDataStream::LittleEndian);
        constexpr quint32 payloadSize = 16000 * 2 * 8;
        data.writeRawData("RIFF", 4);
        data << quint32(payloadSize + 36);
        data.writeRawData("WAVEfmt ", 8);
        data << quint32(16) << quint16(1) << quint16(1) << quint32(16000) << quint32(32000) << quint16(2)
             << quint16(16);
        data.writeRawData("data", 4);
        data << payloadSize;
        wav.append(QByteArray(payloadSize, '\0'));
        return wav;
    }

private slots:
    void remotePlaybackReopensAtSavedPosition()
    {
        NoteEditor              editor;
        MediaPlaybackController playback(&editor);
        if (!playback.available())
            QSKIP("Qt Multimedia is unavailable");

        const auto wav = silentWav();

        MediaReference reference;
        reference.id           = QUuid::createUuid();
        reference.portableName = QStringLiteral("suspend.wav");
        reference.mediaType    = QStringLiteral("audio/wav");
        reference.size         = wav.size();
        editor.setMedia({ reference });
        int reads = 0;
        MediaRangeService::registerResolver(
            &editor, [reference](const MediaReference &r) { return r.id == reference.id; },
            [&wav, &reads](MediaReference, qint64 offset, qint64 length, MediaRangeService::Completion done) {
                ++reads;
                done(wav.mid(offset, length), {});
            });

        QVERIFY(playback.play(reference.uri()));
        QTRY_COMPARE_WITH_TIMEOUT(playback.duration(), qint64(8000), 10000);
        playback.pause();
        QVERIFY(playback.seek(reference.uri(), 1200));
        QTRY_COMPARE(playback.position(), qint64(1200));
        playback.suspend();
        QCOMPARE(playback.position(), qint64(1200));
        QCOMPARE(playback.currentSourceUri(), reference.uri());
        QVERIFY(!playback.playing());
        playback.suspend();
        QCOMPARE(playback.position(), qint64(1200));
        QVERIFY(playback.seek(reference.uri(), 2400));
        QCOMPARE(playback.position(), qint64(2400));
        const int previousReads = reads;
        QVERIFY(playback.toggle(reference.uri()));
        QTRY_VERIFY_WITH_TIMEOUT(reads > previousReads, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(playback.playing(), 10000);
        playback.pause();
        // Qt Multimedia 6.4/GStreamer applies the restored seek asynchronously.
        // Pause before checking so ordinary playback from zero cannot satisfy
        // this assertion just by advancing for 2.4 seconds.
        QTRY_VERIFY_WITH_TIMEOUT(playback.position() >= 2400, 10000);
        playback.stop();
        QVERIFY(playback.currentSourceUri().isEmpty());
        QCOMPARE(playback.position(), qint64(0));
    }

    void seekWaitsForNewFrameAndClearsOnStop()
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        NoteEditor              editor;
        MediaPlaybackController playback(&editor);
        const auto              wav = silentWav();
        MediaReference          reference;
        reference.id           = QUuid::createUuid();
        reference.portableName = QStringLiteral("seek.wav");
        reference.mediaType    = QStringLiteral("audio/wav");
        reference.size         = wav.size();
        editor.setMedia({ reference });
        MediaRangeService::registerResolver(
            &editor, [reference](const MediaReference &r) { return r.id == reference.id; },
            [&wav](MediaReference, qint64 offset, qint64 length, MediaRangeService::Completion done) {
                done(wav.mid(offset, length), {});
            });
        QVERIFY(playback.play(reference.uri()));
        QTRY_COMPARE_WITH_TIMEOUT(playback.duration(), qint64(8000), 10000);
        playback.pause();

        // Control frame delivery independently of the decoder to reproduce the
        // interval between the updated playhead and the first frame after seek.
        QVideoSink sink;
        playback.attachVideoOutput(&sink);
        QVERIFY(playback.seek(reference.uri(), 4000));
        QTRY_COMPARE(playback.position(), qint64(4000));
        QVERIFY(playback.seeking());
        QVideoFrame oldFrame(QVideoFrameFormat(QSize(2, 2), QVideoFrameFormat::Format_RGBA8888));
        oldFrame.setStartTime(0);
        sink.setVideoFrame(oldFrame);
        QVERIFY(playback.seeking());
        QVideoFrame newFrame(QVideoFrameFormat(QSize(2, 2), QVideoFrameFormat::Format_RGBA8888));
        newFrame.setStartTime(4000000);
        sink.setVideoFrame(newFrame);
        QVERIFY(!playback.seeking());

        QVERIFY(playback.seek(reference.uri(), 6000));
        QVERIFY(playback.seeking());
        playback.stop();
        QVERIFY(!playback.seeking());
        playback.detachVideoOutput(&sink);
#else
        QSKIP("Qt Multimedia is unavailable");
#endif
    }
};

QTEST_MAIN(MediaPlaybackSuspendTest)
#include "mediaplaybacksuspend_test.moc"
