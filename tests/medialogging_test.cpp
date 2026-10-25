#include "medialogging.h"

#include <QDebug>
#include <QMutex>
#include <QScopeGuard>
#include <QtTest>

#include <thread>
#include <vector>

namespace {
constexpr const char *MissingPts = "QFFmpeg::Demuxer received AVPacket with pts == AV_NOPTS_VALUE";

struct Message {
    QtMsgType type;
    QString   text, category, function;
    int       line;
};
QMutex         captureMutex;
QList<Message> messages;

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &text)
{
    QMutexLocker lock(&captureMutex);
    messages.append(
        { type, text, QString::fromUtf8(context.category), QString::fromUtf8(context.function), context.line });
}
} // namespace

class MediaLoggingTest : public QObject {
    Q_OBJECT
private slots:
    void limitsOnlyTheRepeatedMissingPtsWarning()
    {
        const auto previous = qInstallMessageHandler(captureMessage);
        const auto restore  = qScopeGuard([previous] { qInstallMessageHandler(previous); });
        AnyKeep::installMediaLogFilter();
        AnyKeep::installMediaLogFilter(); // Initialization is idempotent.

        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([] {
                for (int packet = 0; packet < 100; ++packet)
                    QMessageLogger("qffmpegdemuxer.cpp", 33, "demuxPacket").warning("%s", MissingPts);
            });
        }
        for (auto &worker : workers)
            worker.join();
        qWarning() << MissingPts; // Qt's demuxer uses the stream-style warning.
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().type, QtWarningMsg);
        QVERIFY(messages.first().text.startsWith(QString::fromLatin1(MissingPts)));
        QVERIFY(messages.first().text.contains(QStringLiteral("further identical warnings suppressed")));
        QCOMPARE(messages.first().function, QStringLiteral("demuxPacket"));
        QCOMPARE(messages.first().line, 33);

        QMessageLogger("test", 1, "decode", "qt.multimedia.ffmpeg.demuxer").warning("Demuxing failed");
        qWarning("Different warning");
        qWarning("Different warning");
        qCritical("%s", MissingPts); // Matching text at a higher severity must pass through.
        qInfo("Media info");
        QCOMPARE(messages.size(), 6);
        QCOMPARE(messages.at(1).text, QStringLiteral("Demuxing failed"));
        QCOMPARE(messages.at(1).category, QStringLiteral("qt.multimedia.ffmpeg.demuxer"));
        QCOMPARE(messages.at(2).text, QStringLiteral("Different warning"));
        QCOMPARE(messages.at(3).text, QStringLiteral("Different warning"));
        QCOMPARE(messages.at(4).type, QtCriticalMsg);
        QCOMPARE(messages.at(4).text, QString::fromLatin1(MissingPts));
        QCOMPARE(messages.at(5).type, QtInfoMsg);
        QCOMPARE(messages.at(5).text, QStringLiteral("Media info"));
    }
};

QTEST_APPLESS_MAIN(MediaLoggingTest)
#include "medialogging_test.moc"
