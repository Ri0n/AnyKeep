#include "mediasyncservice.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QThread>
#include <utility>

namespace AnyKeep {

MediaSyncService::MediaSyncService(QObject *parent) : QObject(parent) {}

MediaSyncService *MediaSyncService::instance()
{
    static QPointer<MediaSyncService> service;
    if (!service && QCoreApplication::instance())
        service = new MediaSyncService(QCoreApplication::instance());
    return service;
}

QString MediaSyncService::key(const MediaReference &reference)
{
    // Immutable content identity rather than presentation URI. Two blocks
    // referencing the same bytes must contribute only once to note progress.
    QByteArray identity = reference.checksum;
    if (identity.isEmpty())
        identity = !reference.blobId.isEmpty() ? reference.blobId : reference.id.toRfc4122();
    identity += ':' + QByteArray::number(reference.size);
    const QByteArray instance = reference.remoteData.value(QStringLiteral("xmpp.instance")).toString().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(instance + '\0' + identity,
                                                        QCryptographicHash::Sha256).toHex());
}

void MediaSyncService::registerObserver(QObject *provider, Accepts accepts, Observe observe)
{
    if (!provider)
        return;
    for (auto it = providers_.begin(); it != providers_.end();) {
        if (!it->owner || it->owner == provider)
            it = providers_.erase(it);
        else
            ++it;
    }
    providers_.append({provider, std::move(accepts), std::move(observe)});
    connect(provider, &QObject::destroyed, this, [this] {
        for (auto it = providers_.begin(); it != providers_.end();) {
            if (!it->owner)
                it = providers_.erase(it);
            else
                ++it;
        }
    });
    refreshProvider(provider);
}

void MediaSyncService::dispatch(const MediaReference &reference, QObject *specificProvider)
{
    for (const auto &provider : std::as_const(providers_)) {
        if (!provider.owner || (specificProvider && provider.owner != specificProvider)
            || !provider.accepts(reference))
            continue;
        // Never call plugin logic from shared UI code; dispatch to its owner.
        QMetaObject::invokeMethod(provider.owner,
                                  [guard = provider.owner, observe = provider.observe, reference] {
                                      if (guard)
                                          observe(reference);
                                  }, Qt::QueuedConnection);
        return;
    }
}

void MediaSyncService::refreshProvider(QObject *provider)
{
    for (auto it = watchers_.cbegin(); it != watchers_.cend(); ++it) {
        for (const auto &watch : it.value()) {
            if (watch.owner) {
                dispatch(watch.reference, provider);
                break;
            }
        }
    }
}

void MediaSyncService::watch(QObject *subscriber, const MediaReference &reference)
{
    if (!subscriber || !reference.isValid())
        return;
    const auto id = key(reference);
    auto &entries = watchers_[id];
    for (auto it = entries.begin(); it != entries.end();) {
        if (!it->owner)
            it = entries.erase(it);
        else if (it->owner == subscriber)
            return;
        else
            ++it;
    }
    const bool first = entries.isEmpty();
    entries.append({subscriber, reference});
    if (first)
        dispatch(reference);
    connect(subscriber, &QObject::destroyed, this, [this, subscriber] { unwatch(subscriber); },
            Qt::AutoConnection);
}

void MediaSyncService::unwatch(QObject *subscriber)
{
    for (auto it = watchers_.begin(); it != watchers_.end();) {
        auto &entries = it.value();
        for (auto entry = entries.begin(); entry != entries.end();) {
            if (!entry->owner || entry->owner == subscriber)
                entry = entries.erase(entry);
            else
                ++entry;
        }
        if (entries.isEmpty())
            it = watchers_.erase(it);
        else
            ++it;
    }
}

MediaSyncSnapshot MediaSyncService::snapshot(const MediaReference &reference) const
{
    auto result = snapshots_.value(key(reference));
    result.totalBytes = qMax<qint64>(0, reference.size);
    result.verifiedBytes = qBound<qint64>(0, result.verifiedBytes, result.totalBytes);
    if (result.totalBytes && result.verifiedBytes == result.totalBytes)
        result.state = MediaSyncSnapshot::Complete;
    return result;
}

void MediaSyncService::publish(const MediaReference &reference, MediaSyncSnapshot state)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, reference, state] { publish(reference, state); },
                                  Qt::QueuedConnection);
        return;
    }
    const auto id = key(reference);
    state.totalBytes = qMax<qint64>(0, reference.size);
    state.verifiedBytes = qBound<qint64>(0, state.verifiedBytes, state.totalBytes);
    if (state.totalBytes > 0 && state.verifiedBytes == state.totalBytes)
        state.state = MediaSyncSnapshot::Complete;
    const auto previous = snapshots_.value(id);
    if (previous.totalBytes == state.totalBytes && previous.verifiedBytes == state.verifiedBytes
        && previous.state == state.state && previous.error == state.error)
        return;
    snapshots_[id] = std::move(state);
    emit snapshotChanged(id);
}

} // namespace AnyKeep
