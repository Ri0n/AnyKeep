#ifndef ANYKEEP_MEDIACHUNKSCHEDULER_H
#define ANYKEEP_MEDIACHUNKSCHEDULER_H

#include "anykeep_export.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
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
    using Abort = std::function<void()>;
    using CancelableFetch = std::function<Abort(Completion)>;

    explicit MediaChunkScheduler(QObject *parent = nullptr);
    ~MediaChunkScheduler() override;

    static MediaChunkScheduler *instance();

    // Starts fetch once for an uncached chunk, then distributes its validated
    // result to all registered waiters. Fetch MUST complete exactly once and
    // its Completion may be invoked from any thread.
    quint64 request(QString immutableSource, quint64 index, QString lane, int priority,
                    Fetch fetch, Completion done, QObject *fetchOwner = nullptr);
    // A cancelable provider returns an owner-thread-safe abort request. The
    // active slot remains reserved until its completion acknowledges abort.
    quint64 requestCancelable(QString immutableSource, quint64 index, QString lane, int priority,
                              CancelableFetch fetch, Completion done, QObject *fetchOwner = nullptr);

    // Cancels only this consumer. An already active network operation stays
    // accounted for until the provider completes it; never abort a shared
    // download on behalf of another consumer.
    bool cancel(quint64 ticket);
    // Only after a provider's network worker has stopped: discard an
    // orphaned in-flight request whose completion can no longer arrive.
    // Must not be used as ordinary player cancellation.
    bool abandonStopped(quint64 ticket);
    // Called only AFTER a provider's network worker has stopped. Retires
    // every queued/active fetch owned by it, including canceled jobs with no
    // remaining waiters. Late network completions are safely ignored.
    void retireStoppedOwner(QObject *fetchOwner);
    bool reprioritize(quint64 ticket, int priority);

    void setMaxActive(int count);
    void setLaneMaxActive(const QString &lane, int count);
    // Observe an already negotiated, non-preemptible physical transport.
    // Jingle BUNDLE contents sharing a session count only once; overlapping
    // independent sessions count separately. Observations may temporarily
    // exceed the target global budget instead of disrupting SCTP handovers.
    void observeExternalTransfer(QObject *physicalTransport);
    void releaseExternalTransfer(QObject *physicalTransport);
    int externalActiveCount() const { return externalTransfers_.size(); }
    int activeCount() const { return active_ + externalTransfers_.size(); }
    int pendingCount() const;
    // Optional aggregate *soft* byte-rate limit. A completed HTTP request or
    // an authenticated Jingle record reports its actual network bytes through
    // reportTransferResult(). The limit paces NEW managed fetches; it never
    // suspends an active Jingle/SCTP stream or claims strict wire-rate shaping.
    // Zero disables rate pacing, but keeps transfer/congestion accounting.
    void setBandwidthLimitBytesPerSecond(qint64 bytesPerSecond);
    qint64 bandwidthLimitBytesPerSecond() const { return bandwidthLimitBytesPerSecond_; }

    // Safe to call from a provider thread (the scheduler queues accounting
    // onto its own thread). Report once per physical
    // network response, never once per waiter or local cache hit. Set congested
    // only for transport congestion (timeouts, HTTP 429/5xx), not invalid
    // ETags, authentication failures, or an intentional player cancellation.
    // Congestion adaptively delays new work in the affected lane only.
    void reportTransferResult(const QString &lane, qint64 networkBytes, bool congested);
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
        CancelableFetch cancelableFetch;
        Abort abort;
        bool aborting = false;
        QPointer<QObject> fetchOwner;
        QList<Waiter> waiters;
        bool active = false;
    };
    static QString makeKey(const QString &source, quint64 index);
    quint64 addWaiter(QString immutableSource, quint64 index, QString lane, int priority,
                      Fetch fetch, CancelableFetch cancelableFetch, Completion done,
                      QObject *fetchOwner);
    void schedule();
    void scheduleWakeup(qint64 delayMs);
    void replenishBandwidth();
    void pump();
    void finish(const QString &key, quint64 sequence, QByteArray bytes, QString error);

    QHash<QString, std::shared_ptr<Job>> jobs_;
    QHash<quint64, QString> ticketKeys_;
    QHash<QString, int> laneLimits_;
    QHash<QString, int> laneActive_;
    QSet<QObject *> externalTransfers_;
    quint64 nextTicket_ = 0;
    quint64 nextSequence_ = 0;
    int maxActive_ = 3;
    int active_ = 0;
    // Bound consecutive admissions that bypass the oldest eligible request.
    int priorityBypasses_ = 0;
    struct LaneCongestion {
        int level = 0;
        qint64 retryAfterMs = 0;
    };
    QHash<QString, LaneCongestion> congestion_;
    QElapsedTimer monotonicClock_;
    QTimer admissionTimer_;
    qint64 bandwidthLimitBytesPerSecond_ = 0;
    qint64 lastBudgetUpdateMs_ = 0;
    double bandwidthCredit_ = 0;
    bool scheduled_ = false;
    bool stopped_ = false;
};

} // namespace AnyKeep
#endif // ANYKEEP_MEDIACHUNKSCHEDULER_H
