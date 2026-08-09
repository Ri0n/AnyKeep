#include "speechaudiorecorder.h"

#include <QDebug>

#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
#include <QMediaDevices>
#endif

namespace AnyKeep {

SpeechAudioRecorder::SpeechAudioRecorder(QObject *parent) : QObject(parent)
{
    progressTimer.setInterval(250);
    connect(&progressTimer, &QTimer::timeout, this, [this]() {
        auto ms = elapsedMs();
        emit elapsedChanged(ms, maxDuration);
        if (maxDuration > 0 && ms >= maxDuration) {
            emit maxDurationReached();
        }
    });
}

SpeechAudioRecorder::~SpeechAudioRecorder() { cleanup(); }

bool SpeechAudioRecorder::isAvailable() const
{
#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
    auto device    = QMediaDevices::defaultAudioInput();
    auto available = !device.isNull();
    return available;
#else
    return false;
#endif
}

bool SpeechAudioRecorder::isRecording() const
{
#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
    return audioInput != nullptr;
#else
    return false;
#endif
}

QString SpeechAudioRecorder::errorString() const { return lastError; }

qint64 SpeechAudioRecorder::elapsedMs() const { return elapsed.isValid() ? elapsed.elapsed() : 0; }

bool SpeechAudioRecorder::start(int maxDurationMs)
{
    cleanup();
    lastError.clear();
    maxDuration = maxDurationMs;

#ifndef ANYKEEP_MULTIMEDIA_AVAILABLE
    setError(tr("Audio recording support is not available in this build."));
    return false;
#else
    if (!isAvailable()) {
        setError(tr("No audio input device is available."));
        return false;
    }

    buffer.setData(QByteArray());
    if (!buffer.open(QIODevice::WriteOnly)) {
        setError(tr("Failed to open audio buffer for recording."));
        return false;
    }

    auto requestedFormat = format();
    auto device          = QMediaDevices::defaultAudioInput();
    if (!device.isFormatSupported(requestedFormat)) {
        requestedFormat = device.preferredFormat();
    }
    audioInput = new QAudioSource(device, requestedFormat, this);
    connect(audioInput, &QAudioSource::stateChanged, this, [this](QAudio::State state) {
        if (state == QAudio::StoppedState && audioInput && audioInput->error() != QAudio::NoError) {
            setError(tr("Audio recording failed."));
        }
    });

    audioInput->start(&buffer);
    activeSampleRate    = requestedFormat.sampleRate();
    activeChannels      = requestedFormat.channelCount();
    activeBitsPerSample = requestedFormat.bytesPerSample() * 8;
    elapsed.start();
    progressTimer.start();
    return true;
#endif
}

SpeechRecognitionAudio SpeechAudioRecorder::stop()
{
    SpeechRecognitionAudio audio;
#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
    if (audioInput) {
        audioInput->stop();
    }
#endif
    progressTimer.stop();
    buffer.close();

    audio.data          = buffer.data();
    audio.format        = QLatin1String("audio/l16");
    audio.durationMs    = elapsedMs();
    audio.sampleRate    = activeSampleRate;
    audio.channels      = activeChannels;
    audio.bitsPerSample = activeBitsPerSample;
    cleanup();
    return audio;
}

void SpeechAudioRecorder::cancel()
{
    cleanup();
    buffer.setData(QByteArray());
}

void SpeechAudioRecorder::cleanup()
{
    progressTimer.stop();
#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
    if (audioInput) {
        audioInput->stop();
        audioInput->deleteLater();
        audioInput = nullptr;
    }
#endif
    if (buffer.isOpen()) {
        buffer.close();
    }
    elapsed.invalidate();
    maxDuration = 0;
}

void SpeechAudioRecorder::setError(const QString &error)
{
    lastError = error;
    emit failed(error);
}

#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
QAudioFormat SpeechAudioRecorder::format() const
{
    QAudioFormat format;
    format.setSampleRate(16000);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    return format;
}
#endif

} // namespace AnyKeep
