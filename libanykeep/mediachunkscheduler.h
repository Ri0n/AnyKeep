#ifndef ANYKEEP_MEDIACHUNKSCHEDULER_H
#define ANYKEEP_MEDIACHUNKSCHEDULER_H

#include "anykeep_export.h"
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <functional>
#include <memory>

namespace AnyKeep {

// App-thread admission and single-flight for remote authenticated media records.
// Identity MUST include the provider/account, immutable remote revision and
// ciphertext representation. Each requester owns a cancellable waiter, not a
// network transfer. Providers keep transport verification and cache ownership.
class ANYKEEP_EXPORT MediaChunkScheduler final : public QObject {
public:
    using Completion = std::function<void(QByteArray, QString)>;
    using Fetch = std::function<void(Completion)>;

    explicit MediaChunkScheduler(QObject *parent = nullptr);
    ~MediaChunkScheduler() override;

    static MediaChunkScheduler *instance();

    // Starts fetch once for an uncached chunk, then distributes its validated
    // result to all registered waiters. Fetch MUST complete exactly once and
    // its Completion may be invoked from any thread.
    quint64 request(QString immutableSource, quint64 index, QString lane, int priority,
                    Fetch fetch, Completion done);

    // Cancels only this consumer. An already active network operation stays
    // accounted for until the provider completes it; never abort a shared
    // download on behalf of another consumer.
    bool cancel(quint64 ticket);
    bool reprioritize(quint64 ticket, int priority);

    void setMaxActive(int count);
    void setLaneMaxActive(const QString &lane, int count);
    int activeCount() const { return active_; }
    int pendingCount() const;
    void shutdown();

private:
    struct Waiter {
        quint64 ticket;
        int priority;
        Completion done;
    };
    struct Job {
        QString key;
        QString lane;
        quint64 sequence = 0;
        Fetch fetch;
        QList<Waiter> waiters;
        bool active = false;
    };
    static QString makeKey(const QString &source, quint64 index);
    void schedule();
    void pump();
    void finish(const QString &key, quint64 sequence, QByteArray bytes, QString error);

    QHash<QString, std::shared_ptr<Job>> jobs_;
    QHash<quint64, QString> ticketKeys_;
    QHash<QString, int> laneLimits_;
    QHash<QString, int> laneActive_;
    quint64 nextTicket_ = 0;
    quint64 nextSequence_ = 0;
    int maxActive_ = 3;
    int active_ = 0;
    bool scheduled_ = false;
    bool stopped_ = false;
};

} // namespace AnyKeep
#endif // ANYKEEP_MEDIACHUNKSCHEDULER_H
