#ifndef ANYKEEP_MEDIAPLAYBACKSESSION_H
#define ANYKEEP_MEDIAPLAYBACKSESSION_H

#include "anykeep_export.h"

#include "mediareference.h"
#include <QObject>
#include <QSet>
#include <memory>

namespace AnyKeep {

class ANYKEEP_EXPORT MediaPlaybackSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString currentSourceUri READ currentSourceUri NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY stateChanged)
    Q_PROPERTY(bool hasVideoFrame READ hasVideoFrame NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY stateChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY stateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)

public:
    static MediaPlaybackSession *acquire(const MediaReference &reference, QObject *borrower);
    void                         release(QObject *borrower);
    int                          leaseCount() const { return borrowers_.size(); }
    bool                         matches(const MediaReference &reference) const;
    ~MediaPlaybackSession() override;

    bool    available() const;
    QString currentSourceUri() const;
    bool    playing() const;
    bool    loading() const;
    bool    seeking() const;
    bool    hasVideoFrame() const;
    qint64  position() const;
    qint64  duration() const;
    QString errorString() const;

    Q_INVOKABLE bool play(const QString &sourceUri);
    Q_INVOKABLE bool toggle(const QString &sourceUri);
    Q_INVOKABLE void pause();
    // Release decoder/network state before the mobile process is suspended.
    void             suspend();
    Q_INVOKABLE void stop();
    Q_INVOKABLE bool seek(const QString &sourceUri, qint64 positionMs);
    Q_INVOKABLE void attachVideoOutput(QObject *output);
    Q_INVOKABLE void detachVideoOutput(QObject *output);

signals:
    void stateChanged();

private:
    explicit MediaPlaybackSession(const MediaReference &reference);
    QSet<QObject *> borrowers_;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AnyKeep

#endif // ANYKEEP_MEDIAPLAYBACKSESSION_H
