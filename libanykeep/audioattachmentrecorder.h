#ifndef ANYKEEP_AUDIOATTACHMENTRECORDER_H
#define ANYKEEP_AUDIOATTACHMENTRECORDER_H

#include "anykeep_export.h"
#include "mediareference.h"

#include <QObject>
#include <memory>

namespace AnyKeep {

class ANYKEEP_EXPORT AudioAttachmentRecorder final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool finalizing READ finalizing NOTIFY stateChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)

public:
    explicit AudioAttachmentRecorder(QObject *parent = nullptr);
    ~AudioAttachmentRecorder() override;

    bool    available() const;
    bool    recording() const;
    bool    finalizing() const;
    qint64  duration() const;
    QString errorString() const;

    Q_INVOKABLE bool start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void recordingReady(const AnyKeep::MediaReference &reference, qint64 durationMs);
    void failed(const QString &message);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AnyKeep

#endif // ANYKEEP_AUDIOATTACHMENTRECORDER_H
