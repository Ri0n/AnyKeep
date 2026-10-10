#ifndef ANYKEEP_MEDIASYNCSERVICE_H
#define ANYKEEP_MEDIASYNCSERVICE_H

#include "anykeep_export.h"
#include "mediareference.h"
#include <QObject>
#include <QHash>
#include <QList>
#include <QPointer>
#include <functional>

namespace AnyKeep {

// Installation-local, provider-independent status. Bytes are authenticated,
// durably stored *plaintext* bytes, never wire progress or decoder position.
struct ANYKEEP_EXPORT MediaSyncSnapshot {
    enum State { Waiting = 0, Transferring = 1, Failed = 2, Complete = 3 };
    qint64 verifiedBytes = 0;
    qint64 totalBytes = 0;
    State state = Waiting;
    QString error;
};

class ANYKEEP_EXPORT MediaSyncService final : public QObject {
    Q_OBJECT
public:
    using Accepts = std::function<bool(const MediaReference &)>;
    using Observe = std::function<void(const MediaReference &)>;

    static MediaSyncService *instance();
    static QString key(const MediaReference &reference);

    // Storage owns fetching; opening a note merely asks the provider to
    // observe/restore its cache. Subscriptions are tied to the editor lifetime.
    void registerObserver(QObject *provider, Accepts accepts, Observe observe);
    void refreshProvider(QObject *provider);
    void watch(QObject *subscriber, const MediaReference &reference);
    void unwatch(QObject *subscriber);
    MediaSyncSnapshot snapshot(const MediaReference &reference) const;
    void publish(const MediaReference &reference, MediaSyncSnapshot state);

signals:
    void snapshotChanged(const QString &key);

private:
    explicit MediaSyncService(QObject *parent = nullptr);
    struct Provider {
        QPointer<QObject> owner;
        Accepts accepts;
        Observe observe;
    };
    struct Watch {
        QPointer<QObject> owner;
        MediaReference reference;
    };
    void dispatch(const MediaReference &reference, QObject *specificProvider = nullptr);
    QList<Provider> providers_;
    QHash<QString, QList<Watch>> watchers_;
    QHash<QString, MediaSyncSnapshot> snapshots_;
};

} // namespace AnyKeep
#endif
