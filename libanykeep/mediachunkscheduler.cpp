#include "mediachunkscheduler.h"

#include <QCoreApplication>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace AnyKeep {

MediaChunkScheduler::MediaChunkScheduler(QObject *parent) : QObject(parent)
{
    monotonicClock_.start();
    admissionTimer_.setSingleShot(true);
    connect(&admissionTimer_, &QTimer::timeout, this, [this] { schedule(); });
}
MediaChunkScheduler::~MediaChunkScheduler() { shutdown(); }

MediaChunkScheduler *MediaChunkScheduler::instance()
{
    static QPointer<MediaChunkScheduler> scheduler;
    if (!scheduler && QCoreApplication::instance()) {
        auto *created = new MediaChunkScheduler(QCoreApplication::instance());
        // Settings apply to the shared application instance. Directly
        // constructed test schedulers retain the reproducible unlimited default.
        created->setBandwidthLimitBytesPerSecond(
            QSettings().value(QStringLiteral("media/download-bytes-per-second"), 0).toLongLong());
        scheduler = created;
    }
    return scheduler;
}

QString MediaChunkScheduler::makeKey(const QString &source, quint64 index)
{
    // Prefix the source length so distinct user-supplied identities cannot
    // accidentally collide with the numeric chunk suffix.
    return QString::number(source.size()) + QLatin1Char(':') + source + QLatin1Char(':')
        + QString::number(index);
}

quint64 MediaChunkScheduler::request(QString immutableSource, quint64 index, QString lane, int priority,
                                      Fetch fetch, Completion done, QObject *fetchOwner)
{
    return addWaiter(std::move(immutableSource), index, std::move(lane), priority,
                     std::move(fetch), {}, std::move(done), fetchOwner);
}

quint64 MediaChunkScheduler::requestCancelable(QString immutableSource, quint64 index, QString lane, int priority,
                                                CancelableFetch fetch, Completion done, QObject *fetchOwner)
{
    return addWaiter(std::move(immutableSource), index, std::move(lane), priority,
                     {}, std::move(fetch), std::move(done), fetchOwner);
}

quint64 MediaChunkScheduler::addWaiter(QString immutableSource, quint64 index, QString lane, int priority,
                                        Fetch fetch, CancelableFetch cancelableFetch, Completion done,
                                        QObject *fetchOwner)
{
    if (stopped_ || immutableSource.isEmpty() || lane.isEmpty() || (!fetch && !cancelableFetch) || !done) {
        if (done)
            done({}, QStringLiteral("Media chunk scheduler unavailable or invalid request"));
        return 0;
    }

    const auto key = makeKey(immutableSource, index);
    auto job = jobs_.value(key);
    if (job && job->lane != lane) {
        done({}, QStringLiteral("Conflicting transport lane for one immutable media chunk"));
        return 0;
    }
    if (!job) {
        job = std::make_shared<Job>();
        job->key = key;
        job->lane = std::move(lane);
        job->sequence = ++nextSequence_;
        job->fetch = std::move(fetch);
        job->cancelableFetch = std::move(cancelableFetch);
        job->fetchOwner = fetchOwner;
        jobs_.insert(key, job);
    }
    const quint64 ticket = ++nextTicket_;
    job->waiters.append({ticket, priority, std::move(done)});
    ticketKeys_.insert(ticket, key);
    schedule();
    return ticket;
}

bool MediaChunkScheduler::cancel(quint64 ticket)
{
    const auto key = ticketKeys_.take(ticket);
    if (key.isEmpty())
        return false;
    const auto job = jobs_.value(key);
    if (!job)
        return false;
    Completion done;
    for (auto it = job->waiters.begin(); it != job->waiters.end(); ++it) {
        if (it->ticket == ticket) {
            done = std::move(it->done);
            job->waiters.erase(it);
            break;
        }
    }
    if (job->waiters.isEmpty()) {
        if (!job->active)
            jobs_.remove(key);
        else if (job->abort && !job->aborting) {
            // Keep the physical network slot occupied until the transport
            // reports completion. A new waiter arriving during abort is
            // rescheduled if the aborted transfer does not return valid data.
            job->aborting = true;
            job->abort();
        }
    }
    schedule();
    if (done)
        done({}, QStringLiteral("Media chunk request cancelled"));
    return true;
}

bool MediaChunkScheduler::abandonStopped(quint64 ticket)
{
    const auto key = ticketKeys_.value(ticket);
    if (key.isEmpty())
        return false;
    const auto job = jobs_.value(key);
    if (!job)
        return false;
    // Remove this consumer and deliver its cancellation exactly once.
    const bool removed = cancel(ticket);
    if (!removed || !job->waiters.isEmpty() || !job->active)
        return removed;
    // The transport has already been destroyed, so no future completion
    // will settle this orphaned job. Release the reserved network slot.
    if (jobs_.value(key) != job)
        return removed;
    jobs_.remove(key);
    --active_;
    if (--laneActive_[job->lane] == 0)
        laneActive_.remove(job->lane);
    schedule();
    return removed;
}

void MediaChunkScheduler::retireStoppedOwner(QObject *fetchOwner)
{
    if (!fetchOwner)
        return;
    QList<std::shared_ptr<Job>> retired;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const auto &job = it.value();
        if (job->fetchOwner != fetchOwner) {
            ++it;
            continue;
        }
        retired.append(job);
        if (job->active) {
            --active_;
            if (--laneActive_[job->lane] == 0)
                laneActive_.remove(job->lane);
        }
        for (const auto &waiter : std::as_const(job->waiters))
            ticketKeys_.remove(waiter.ticket);
        it = jobs_.erase(it);
    }
    schedule();
    // Only settle after removing all jobs: a callback may reenter request().
    for (const auto &job : std::as_const(retired)) {
        for (auto &waiter : job->waiters) {
            if (waiter.done)
                waiter.done({}, QStringLiteral("Media transport stopped"));
        }
    }
}

bool MediaChunkScheduler::reprioritize(quint64 ticket, int priority)
{
    const auto job = jobs_.value(ticketKeys_.value(ticket));
    if (!job)
        return false;
    for (auto &waiter : job->waiters) {
        if (waiter.ticket == ticket) {
            waiter.priority = priority;
            schedule();
            return true;
        }
    }
    return false;
}

void MediaChunkScheduler::observeExternalTransfer(QObject *physicalTransport)
{
    if (!physicalTransport || stopped_ || externalTransfers_.contains(physicalTransport))
        return;
    externalTransfers_.insert(physicalTransport);
    // Neither cancellation nor later scheduler destruction should ever
    // delete or terminate a Jingle session. Merely track its real lifetime.
    connect(physicalTransport, &QObject::destroyed, this, [this, physicalTransport] {
        releaseExternalTransfer(physicalTransport);
    });
}

void MediaChunkScheduler::releaseExternalTransfer(QObject *physicalTransport)
{
    if (externalTransfers_.remove(physicalTransport))
        schedule();
}

void MediaChunkScheduler::setMaxActive(int count)
{
    maxActive_ = std::max(1, count);
    schedule();
}

void MediaChunkScheduler::setLaneMaxActive(const QString &lane, int count)
{
    if (!lane.isEmpty()) {
        laneLimits_.insert(lane, std::max(1, count));
        schedule();
    }
}

void MediaChunkScheduler::setBandwidthLimitBytesPerSecond(qint64 bytesPerSecond)
{
    replenishBandwidth();
    const qint64 next = std::max<qint64>(0, bytesPerSecond);
    if (next == bandwidthLimitBytesPerSecond_)
        return;
    if (!bandwidthLimitBytesPerSecond_)
        bandwidthCredit_ = double(next); // One second of initial burst.
    else
        bandwidthCredit_ = std::min(bandwidthCredit_, double(next));
    bandwidthLimitBytesPerSecond_ = next;
    lastBudgetUpdateMs_ = monotonicClock_.elapsed();
    admissionTimer_.stop();
    schedule();
}

void MediaChunkScheduler::replenishBandwidth()
{
    const qint64 now = monotonicClock_.elapsed();
    const qint64 elapsed = now - lastBudgetUpdateMs_;
    if (elapsed > 0 && bandwidthLimitBytesPerSecond_ > 0) {
        bandwidthCredit_ = std::min(double(bandwidthLimitBytesPerSecond_),
                                     bandwidthCredit_ + double(elapsed) * double(bandwidthLimitBytesPerSecond_) / 1000.0);
    }
    lastBudgetUpdateMs_ = now;
}

void MediaChunkScheduler::reportTransferResult(const QString &lane, qint64 networkBytes, bool congested)
{
    if (stopped_)
        return;
    replenishBandwidth();
    if (networkBytes > 0 && bandwidthLimitBytesPerSecond_ > 0)
        bandwidthCredit_ -= double(networkBytes);

    if (!lane.isEmpty()) {
        if (congested) {
            auto &state = congestion_[lane];
            state.level = std::min(6, state.level + 1);
            // Lane-scoped bounded exponential cooldown (0.5s .. 8s).
            const qint64 delay = std::min<qint64>(8000, 500LL << (state.level - 1));
            state.retryAfterMs = monotonicClock_.elapsed() + delay;
        } else {
            auto it = congestion_.find(lane);
            if (it != congestion_.end()) {
                if (--it->level <= 0)
                    congestion_.erase(it);
            }
        }
    }
    schedule();
}

int MediaChunkScheduler::pendingCount() const
{
    int result = 0;
    for (const auto &job : jobs_) {
        if (!job->active && !job->waiters.isEmpty())
            ++result;
    }
    return result;
}

void MediaChunkScheduler::schedule()
{
    if (stopped_ || scheduled_)
        return;
    scheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        scheduled_ = false;
        pump();
    });
}

void MediaChunkScheduler::scheduleWakeup(qint64 delayMs)
{
    // One timer for all lanes and bandwidth pacing. Never replace an earlier
    // wakeup with a later one; a new urgent waiter can still call schedule().
    const int waitMs = int(std::clamp<qint64>(delayMs, 1, std::numeric_limits<int>::max()));
    if (!admissionTimer_.isActive() || admissionTimer_.remainingTime() > waitMs)
        admissionTimer_.start(waitMs);
}

void MediaChunkScheduler::pump()
{
    if (stopped_)
        return;
    replenishBandwidth();
    if (bandwidthLimitBytesPerSecond_ > 0 && bandwidthCredit_ < 0) {
        if (pendingCount() > 0) {
            // A very low configured rate combined with a large external
            // Jingle burst can exceed qint64 milliseconds. Clamp the wakeup;
            // the next timer tick will re-check the remaining debt.
            const double waitMs = std::ceil(-bandwidthCredit_ * 1000.0
                                            / double(bandwidthLimitBytesPerSecond_));
            scheduleWakeup(waitMs >= double(std::numeric_limits<int>::max())
                               ? std::numeric_limits<int>::max() : qint64(waitMs));
        }
        return;
    }
    while (active_ + externalTransfers_.size() < maxActive_) {
        std::shared_ptr<Job> candidate;
        std::shared_ptr<Job> oldest;
        qint64 firstLaneWake = std::numeric_limits<qint64>::max();
        const qint64 now = monotonicClock_.elapsed();
        int bestPriority = std::numeric_limits<int>::min();
        for (const auto &job : jobs_) {
            if (job->active || job->waiters.isEmpty()
                || laneActive_.value(job->lane) >= laneLimits_.value(job->lane, 1))
                continue;
            const auto congestion = congestion_.constFind(job->lane);
            if (congestion != congestion_.cend() && congestion->retryAfterMs > now) {
                firstLaneWake = std::min(firstLaneWake, congestion->retryAfterMs - now);
                continue;
            }
            if (!oldest || job->sequence < oldest->sequence)
                oldest = job;
            int priority = std::numeric_limits<int>::min();
            for (const auto &waiter : job->waiters)
                priority = std::max(priority, waiter.priority);
            if (!candidate || priority > bestPriority
                || (priority == bestPriority && job->sequence < candidate->sequence)) {
                candidate = job;
                bestPriority = priority;
            }
        }
        if (!candidate) {
            if (firstLaneWake != std::numeric_limits<qint64>::max())
                scheduleWakeup(firstLaneWake);
            break;
        }
        if (firstLaneWake != std::numeric_limits<qint64>::max())
            scheduleWakeup(firstLaneWake);
        // Bound starvation without interrupting active Jingle/SCTP transfers.
        constexpr int maxPriorityBypasses = 4;
        if (candidate != oldest && priorityBypasses_ >= maxPriorityBypasses)
            candidate = oldest;
        if (candidate == oldest)
            priorityBypasses_ = 0;
        else
            ++priorityBypasses_;
        candidate->active = true;
        ++active_;
        ++laneActive_[candidate->lane];

        const QString key = candidate->key;
        const quint64 sequence = candidate->sequence;
        const QPointer<MediaChunkScheduler> guard(this);
        auto complete = [guard, key, sequence](QByteArray bytes, QString error) mutable {
            if (!guard)
                return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, key, sequence, bytes = std::move(bytes), error = std::move(error)]() mutable {
                    if (guard)
                        guard->finish(key, sequence, std::move(bytes), std::move(error));
                },
                Qt::QueuedConnection);
        };
        if (candidate->cancelableFetch) {
            candidate->abort = candidate->cancelableFetch(std::move(complete));
            if (candidate->waiters.isEmpty() && candidate->abort && !candidate->aborting) {
                candidate->aborting = true;
                candidate->abort();
            }
        } else {
            candidate->fetch(std::move(complete));
        }
    }
}

void MediaChunkScheduler::finish(const QString &key, quint64 sequence, QByteArray bytes, QString error)
{
    auto job = jobs_.value(key);
    if (!job || job->sequence != sequence || !job->active)
        return; // Late or duplicated completion after shutdown/retry.
    --active_;
    if (--laneActive_[job->lane] == 0)
        laneActive_.remove(job->lane);
    if (job->aborting && !job->waiters.isEmpty() && (!error.isEmpty() || bytes.isEmpty())) {
        // Consumers may have joined while a now-unneeded transfer was being
        // aborted. Restart it as a new admitted job rather than reporting a
        // cancellation that belonged to a previous consumer.
        job->active = false;
        job->aborting = false;
        job->abort = {};
        job->sequence = ++nextSequence_;
        schedule();
        return;
    }
    jobs_.remove(key);
    for (const auto &waiter : std::as_const(job->waiters))
        ticketKeys_.remove(waiter.ticket);
    auto waiters = std::move(job->waiters);
    schedule();
    for (auto &waiter : waiters) {
        if (waiter.done)
            waiter.done(bytes, error);
    }
}

void MediaChunkScheduler::shutdown()
{
    if (stopped_)
        return;
    stopped_ = true;
    admissionTimer_.stop();
    congestion_.clear();
    auto jobs = std::move(jobs_);
    jobs_.clear();
    ticketKeys_.clear();
    laneActive_.clear();
    externalTransfers_.clear();
    active_ = 0;
    for (const auto &job : std::as_const(jobs)) {
        for (const auto &waiter : std::as_const(job->waiters)) {
            if (waiter.done)
                waiter.done({}, QStringLiteral("Media chunk scheduler stopped"));
        }
    }
}

} // namespace AnyKeep
