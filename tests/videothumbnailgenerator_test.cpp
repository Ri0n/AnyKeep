#include "localmediastore.h"
#include "mediareference.h"
#include "secureenvelope.h"
#include "videothumbnailgenerator.h"

#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <memory>

using namespace AnyKeep;

class VideoThumbnailGeneratorTest : public QObject {
    Q_OBJECT
private slots:
    void candidatePositionsStayInFirstHalf()
    {
        const auto longVideo = VideoThumbnailGenerator::candidatePositions(20000);
        QVERIFY(!longVideo.isEmpty());
        QCOMPARE(longVideo.first(), qint64(2000));
        for (const auto position : longVideo)
            QVERIFY(position >= 0 && position <= 10000);

        const auto shortVideo = VideoThumbnailGenerator::candidatePositions(1900);
        QVERIFY(!shortVideo.isEmpty());
        QCOMPARE(shortVideo.first(), qint64(950));
        for (const auto position : shortVideo)
            QVERIFY(position >= 0 && position <= 950);
        QCOMPARE(VideoThumbnailGenerator::candidatePositions(0), QVector<qint64>({ 0 }));
    }

    void boundsDecodedJpegThumbnail()
    {
        QImage image(640, 360, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                image.setPixel(x, y, qRgb((x * 17 + y) % 256, (y * 11 + x / 2) % 256, (x * 3 + y * 7) % 256));
            }
        }
        const QByteArray jpeg = VideoThumbnailGenerator::encodeTinyJpeg(image);
        QVERIFY2(!jpeg.isEmpty(), "JPEG thumbnail encoding failed");
        QVERIFY(jpeg.size() <= MediaReference::MaxVideoThumbnailBytes);
        QImage decoded;
        QVERIFY(decoded.loadFromData(jpeg, "JPEG"));
        QVERIFY(MediaReference::isValidVideoThumbnail(jpeg));
        QVERIFY(decoded.width() <= 240);
        QVERIFY(decoded.height() <= 240);
        QVERIFY(decoded.width() > 0 && decoded.height() > 0);

        const auto poster = VideoThumbnailGenerator::encodePosterJpeg(image);
        QVERIFY2(!poster.isEmpty(), "Full poster encoding failed");
        QVERIFY(poster.size() <= 128 * 1024);
        QVERIFY(poster.size() > jpeg.size());
        QImage decodedPoster;
        QVERIFY(decodedPoster.loadFromData(poster, "JPEG"));
        QCOMPARE(decodedPoster.width(), 640);
        QCOMPARE(decodedPoster.height(), 360);
    }

    void emptyImageHasNoThumbnail() { QVERIFY(VideoThumbnailGenerator::encodeTinyJpeg({}).isEmpty()); }

    void portraitThumbnailsBoundBothDimensions()
    {
        QImage image(360, 1920, QImage::Format_RGB32);
        image.fill(Qt::blue);
        const auto tiny = VideoThumbnailGenerator::encodeTinyJpeg(image);
        QVERIFY(MediaReference::isValidVideoThumbnail(tiny));
        const auto decoded = QImage::fromData(tiny, "JPEG");
        QCOMPARE(decoded.height(), 240);
        QVERIFY(qAbs(double(decoded.width()) / decoded.height() - 360.0 / 1920.0) < 0.01);
        const auto poster = QImage::fromData(VideoThumbnailGenerator::encodePosterJpeg(image), "JPEG");
        QCOMPARE(poster.height(), 640);
        QVERIFY(poster.width() <= 640);
        QVERIFY(!MediaReference::isValidVideoThumbnail(QByteArray::fromHex("ffd8ffd9")));
    }

    void cancelledRequestDoesNotHoldTheQueueOrDeliverCompletion()
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LocalMediaStore store(directory.path(), SecureEnvelope::generateMasterKey());
        MediaReference  missing;
        missing.id           = QUuid::createUuid();
        missing.portableName = QStringLiteral("missing.mp4");
        missing.mediaType    = QStringLiteral("video/mp4");
        int  cancelledCalls  = 0;
        auto lifetime        = std::make_unique<QObject>();
        VideoThumbnailGenerator::start(
            missing, lifetime.get(), [&](qint64, QByteArray, QByteArray) { ++cancelledCalls; }, &store);
        lifetime.reset();

        int completedCalls = 0;
        VideoThumbnailGenerator::start(
            missing, this,
            [&](qint64 position, QByteArray tiny, QByteArray poster) {
                QCOMPARE(QThread::currentThread(), thread());
                QCOMPARE(position, qint64(-1));
                QVERIFY(tiny.isEmpty());
                QVERIFY(poster.isEmpty());
                ++completedCalls;
            },
            &store);
        QCOMPARE(completedCalls, 0);
        QTRY_COMPARE(completedCalls, 1);
        QCOMPARE(cancelledCalls, 0);
#else
        QSKIP("Qt Multimedia is unavailable");
#endif
    }
};

QTEST_MAIN(VideoThumbnailGeneratorTest)
#include "videothumbnailgenerator_test.moc"
