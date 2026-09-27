#ifndef ANYKEEP_MEDIAPLAYBACKCONTROLLER_H
#define ANYKEEP_MEDIAPLAYBACKCONTROLLER_H

#include "anykeep_export.h"

#include <QObject>
#include <memory>

namespace AnyKeep {

class NoteEditor;

class ANYKEEP_EXPORT MediaPlaybackController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString currentSourceUri READ currentSourceUri NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY stateChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    Q_PROPERTY(QObject *videoSink READ videoSink CONSTANT)

public:
    explicit MediaPlaybackController(NoteEditor *editor, QObject *parent = nullptr);
    ~MediaPlaybackController() override;

    bool    available() const;
    QString currentSourceUri() const;
    bool    playing() const;
    bool    loading() const;
    qint64  position() const;
    qint64  duration() const;
    QString errorString() const;

    Q_INVOKABLE bool play(const QString &sourceUri);
    Q_INVOKABLE bool toggle(const QString &sourceUri);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE bool seek(const QString &sourceUri, qint64 positionMs);

signals:
    void stateChanged();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AnyKeep

#endif // ANYKEEP_MEDIAPLAYBACKCONTROLLER_H
