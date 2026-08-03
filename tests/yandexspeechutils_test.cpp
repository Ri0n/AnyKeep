#include <QtEndian>
#include <QtTest>

#include "yandexspeechutils.h"

using namespace AnyKeep;

class YandexSpeechUtilsTest : public QObject {
    Q_OBJECT

private slots:
    void parsesStreamAndUsesNormalizedRefinement();
    void parsesWrappedStreamWithJsonCharactersInText();
    void reportsStatusWarnings();
    void detectsOnlySafeDirectContainers();
    void resamplesMonoPcmAcrossChunks();
    void drainsResamplerWithoutResetting();
    void rejectsOversizedPcm();
};

void YandexSpeechUtilsTest::parsesStreamAndUsesNormalizedRefinement()
{
    const QByteArray response = R"({"result":{"audioCursors":{"finalIndex":"1"},)"
                                R"("final":{"alternatives":[{"text":"первый сырой"}]}}})"
                                "\n"
                                R"({"result":{"audioCursors":{"finalIndex":"2"},)"
                                R"("final":{"alternatives":[{"text":"второй"}]}}})"
                                "\n"
                                R"({"result":{"finalRefinement":{"finalIndex":"1",)"
                                R"("normalizedText":{"alternatives":[{"text":"Первый, нормализованный."}]}}}})";

    const QString transcript = YandexSpeech::transcriptFromResult(response);
    QCOMPARE(transcript, QStringLiteral("Первый, нормализованный. второй"));
}

void YandexSpeechUtilsTest::parsesWrappedStreamWithJsonCharactersInText()
{
    const QByteArray response = R"({"result":{"audioCursors":{"finalIndex":"7"},)"
                                R"("final":{"alternatives":[{"text":"объект {тест} и \"кавычки\""}]}}})"
                                "\n"
                                R"({"result":{"statusCode":{"codeType":"CLOSED","message":"done"}}})";

    const QString transcript = YandexSpeech::transcriptFromResult(response);
    QCOMPARE(transcript, QStringLiteral("объект {тест} и \"кавычки\""));
}

void YandexSpeechUtilsTest::reportsStatusWarnings()
{
    QString          error;
    const QByteArray response = R"({"result":{"statusCode":{"codeType":"WARNING","message":"language fallback"}}})";
    QVERIFY(YandexSpeech::transcriptFromResult(response, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("language fallback"));
}

void YandexSpeechUtilsTest::detectsOnlySafeDirectContainers()
{
    using Type = YandexSpeech::ContainerAudioType;
    QVERIFY(YandexSpeech::directContainerAudioType(QStringLiteral("audio/mpeg"), QString(), 0) == Type::Mp3);
    QVERIFY(YandexSpeech::directContainerAudioType(QStringLiteral("audio/opus"), QString(), 0) == Type::OggOpus);
    QVERIFY(YandexSpeech::directContainerAudioType(QStringLiteral("audio/ogg"), QStringLiteral("voice.ogg"), 0)
            == Type::None);
    QVERIFY(YandexSpeech::directContainerAudioType(QStringLiteral("audio/wav"), QString(), 16) == Type::Wav);
    QVERIFY(YandexSpeech::directContainerAudioType(QStringLiteral("audio/wav"), QString(), 24) == Type::None);
}

void YandexSpeechUtilsTest::resamplesMonoPcmAcrossChunks()
{
    YandexSpeech::MonoPcm16Resampler resampler(2, 32);
    QVERIFY(resampler.append({ -1.0f, 0.0f }, 4));
    QVERIFY(resampler.append({ 1.0f, 0.0f }, 4));
    const QByteArray pcm = resampler.takeResult();
    QCOMPARE(pcm.size(), 4);
    QCOMPARE(qint16(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(pcm.constData()))), qint16(-32767));
    QCOMPARE(qint16(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(pcm.constData() + 2))), qint16(32767));
}

void YandexSpeechUtilsTest::drainsResamplerWithoutResetting()
{
    YandexSpeech::MonoPcm16Resampler resampler(2, 8);
    QVERIFY(resampler.append({ -1.0f, 0.0f }, 4));
    QCOMPARE(resampler.takeAvailable().size(), 2);
    QVERIFY(resampler.append({ 1.0f, 0.0f }, 4));
    const QByteArray tail = resampler.takeResult();
    QCOMPARE(tail.size(), 2);
    QCOMPARE(qint16(qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(tail.constData()))), qint16(32767));
}

void YandexSpeechUtilsTest::rejectsOversizedPcm()
{
    YandexSpeech::MonoPcm16Resampler resampler(100, 100);
    QVERIFY(!resampler.append(QVector<float>(100, 0.0f), 100));
    QVERIFY(!resampler.errorString().isEmpty());
}

QTEST_MAIN(YandexSpeechUtilsTest)
#include "yandexspeechutils_test.moc"
