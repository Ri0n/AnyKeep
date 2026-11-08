#include "mediareference.h"
#include "videothumbnailgenerator.h"

#include <QImage>
#include <QtTest>

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
                image.setPixel(x, y, qRgb((x * 17 + y) % 256, (y * 11 + x / 2) % 256,
                                         (x * 3 + y * 7) % 256));
            }
        }
        const QByteArray jpeg = VideoThumbnailGenerator::encodeTinyJpeg(image);
        QVERIFY2(!jpeg.isEmpty(), "JPEG thumbnail encoding failed");
        QVERIFY(jpeg.size() <= MediaReference::MaxVideoThumbnailBytes);
        QImage decoded;
        QVERIFY(decoded.loadFromData(jpeg, "JPEG"));
        QVERIFY(decoded.width() <= 240);
        QVERIFY(decoded.height() <= 240);
        QVERIFY(decoded.width() > 0 && decoded.height() > 0);
    }

    void emptyImageHasNoThumbnail()
    {
        QVERIFY(VideoThumbnailGenerator::encodeTinyJpeg({}).isEmpty());
    }
};

QTEST_MAIN(VideoThumbnailGeneratorTest)
#include "videothumbnailgenerator_test.moc"
