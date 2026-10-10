#include "irismediasync.h"
#include "irisjinglepublicationprovider.h"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <algorithm>
#include <iris/jingle-pub.h>
#include <iris/jingle-session.h>
#include <iris/xmpp_client.h>
#include <utility>

namespace AnyKeep {
IrisMediaChunkSink::IrisMediaChunkSink(MediaChunkWireParameters parameters, IrisMediaGapMap::Gap gap, Commit commit,
                                       QObject *parent) :
    QIODevice(parent), parameters_(std::move(parameters)), next_(gap.first), end_(gap.first + gap.count),
    commit_(std::move(commit))
{
    if (gap.first >= parameters_.chunkCount() || !gap.count || gap.count > parameters_.chunkCount() - gap.first)
        return;
    const auto begin = MediaChunkWire::wireChunkOffset(parameters_, next_);
    const auto end   = end_ == parameters_.chunkCount() ? MediaChunkWire::wireSize(parameters_)
                                                        : MediaChunkWire::wireChunkOffset(parameters_, end_);
    if (gap.count && begin && end && *end >= *begin) {
        limit_ = *end - *begin;
        open(QIODevice::WriteOnly);
    }
}

qint64 IrisMediaChunkSink::writeData(const char *data, qint64 size)
{
    if (size < 0 || quint64(size) > limit_ - received_) {
        setErrorString(QStringLiteral("Peer exceeded the requested encrypted gap"));
        return -1;
    }
    received_ += quint64(size);
    if (paused_)
        return size;
    qint64 cursor = 0;
    while (cursor < size) {
        const auto expected = MediaChunkWire::wireChunkSize(parameters_, next_);
        if (!expected || next_ >= end_)
            return -1;
        const auto take = qMin<qint64>(size - cursor, qint64(*expected) - record_.size());
        record_.append(data + cursor, take);
        cursor += take;
        if (quint64(record_.size()) != *expected)
            continue;
        const auto opened = MediaChunkWire::decryptChunk(parameters_, next_, record_);
        if (!opened) {
            setErrorString(opened.error);
            return -1;
        }
        const auto action = commit_(next_, record_, opened.value);
        if (action == Action::Error) {
            setErrorString(QStringLiteral("Cannot commit authenticated media chunk"));
            return -1;
        }
        ++next_;
        record_.clear();
        if (action == Action::Pause) {
            paused_ = true;
            break;
        }
    }
    return size;
}
}

#ifdef IRIS_FT_DEFERRED_RECEIPTS
namespace AnyKeep {
namespace J  = XMPP::Jingle;
namespace FT = XMPP::Jingle::FileTransfer;

class IrisMediaSync::Impl {
public:
    struct Part {
        IrisMediaGapMap::Gap         gap;
        QPointer<J::Session>         session;
        QPointer<FT::Application>    app;
        QPointer<IrisMediaChunkSink> sink;
        bool accepted = false, verified = false, draining = false, drained = false, closing = false;
        bool shared = false;
    };
    IrisMediaSync                                       *q;
    QPointer<XMPP::Client>                               client;
    J::JinglePub                                         publication;
    MediaChunkWireParameters                             parameters;
    QString                                              directory;
    MediaReference                                       reference;
    quint64                                              verifiedPlain = 0;
    bool                                                 transferring = false;
    IrisMediaGapMap                                      map;
    quint64                                              priority = 0;
    QHash<quint64, QList<MediaRangeService::Completion>> waiters;
    QList<std::shared_ptr<Part>>                         parts;
    std::shared_ptr<Part>                                current, pending;
    QPointer<J::PublishedSessionRequest>                 request;
    QMetaObject::Connection                              incoming;
    QTimer                                              *timer;
    QStringList                                          restore;
    bool    scanning = true, scheduled = false, stopped = false, demandPending = true;
    QString requestedSid;
    QString error;
    int     timeout;

    Impl(IrisMediaSync *owner, XMPP::Client *c, J::JinglePub pub, MediaChunkWireParameters p, QString dir,
         int timeoutMs, MediaReference ref) :
        q(owner), client(c), publication(std::move(pub)), parameters(std::move(p)), directory(std::move(dir)),
        reference(std::move(ref)),
        map(parameters.chunkCount()), timer(new QTimer(owner)), timeout(qBound(1000, timeoutMs, 25000))
    {
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, q, [this] { fail(QStringLiteral("Media gap handover timed out")); });
        restore = QDir(directory).entryList(QDir::Files, QDir::Name);
        QTimer::singleShot(0, q, [this] { restoreNext(); });
    }

    void report()
    {
        if (!reference.isValid())
            return;
        MediaSyncSnapshot snapshot;
        snapshot.verifiedBytes = qint64(qMin(verifiedPlain, parameters.plainSize));
        snapshot.totalBytes = qint64(parameters.plainSize);
        snapshot.state = map.complete() ? MediaSyncSnapshot::Complete
                       : stopped ? MediaSyncSnapshot::Waiting
                       : !error.isEmpty() ? MediaSyncSnapshot::Failed
                       : transferring ? MediaSyncSnapshot::Transferring
                                       : MediaSyncSnapshot::Waiting;
        if (snapshot.state == MediaSyncSnapshot::Failed)
            snapshot.error = error;
        MediaSyncService::instance()->publish(reference, snapshot);
    }

    void markAvailable(quint64 index)
    {
        if (map.contains(index))
            return;
        map.markAvailable(index);
        verifiedPlain += MediaChunkWire::plainChunkSize(parameters, index).value_or(0);
        report();
    }

    void invalidate(quint64 index)
    {
        if (!map.contains(index))
            return;
        map.invalidate(index);
        verifiedPlain -= MediaChunkWire::plainChunkSize(parameters, index).value_or(0);
        report();
    }

    QString    path(quint64 index) const { return directory + '/' + QString::number(index); }
    QByteArray load(quint64 index)
    {
        const auto expected = MediaChunkWire::wireChunkSize(parameters, index);
        QFile      file(path(index));
        if (!expected || !file.open(QIODevice::ReadOnly)) {
            invalidate(index);
            return {};
        }
        if (quint64(file.size()) == *expected) {
            auto bytes = file.readAll();
            if (MediaChunkWire::decryptChunk(parameters, index, bytes)) {
                markAvailable(index);
                return bytes;
            }
        }
        file.close();
        file.remove();
        invalidate(index);
        return {};
    }

    void restoreNext()
    {
        if (stopped)
            return;
        // Authenticate persisted records without one long GUI-thread disk scan.
        for (int batch = 0; batch < 4 && !restore.isEmpty(); ++batch) {
            bool       ok    = false;
            const auto index = restore.takeLast().toULongLong(&ok);
            if (ok && index < parameters.chunkCount())
                load(index);
        }
        if (!restore.isEmpty())
            QTimer::singleShot(0, q, [this] { restoreNext(); });
        else {
            scanning = false;
            report();
            schedule();
        }
    }

    void deliver(quint64 index, QByteArray wire)
    {
        QTimer::singleShot(0, q, [guard = QPointer<IrisMediaSync>(q), index, wire = std::move(wire)] {
            if (!guard)
                return;
            const auto callbacks = guard->d->waiters.take(index);
            for (const auto &callback : callbacks) {
                if (!guard)
                    break;
                callback(wire, {});
            }
        });
    }

    void read(quint64 index, MediaRangeService::Completion callback)
    {
        if (index >= parameters.chunkCount()) {
            callback({}, QStringLiteral("Invalid media chunk"));
            return;
        }
        if (stopped) {
            callback({}, QStringLiteral("Media synchronization cancelled"));
            return;
        }
        // A subsequent demand may retry a transient failure; committed chunks survive.
        if (!error.isEmpty())
            error.clear();
        waiters[index].append(std::move(callback));
        priority      = index;
        demandPending = true;
        if (auto bytes = load(index); !bytes.isEmpty())
            deliver(index, std::move(bytes));
        schedule();
    }

    void schedule()
    {
        if (scheduled || stopped || !error.isEmpty())
            return;
        scheduled = true;
        QTimer::singleShot(0, q, [this] {
            scheduled = false;
            plan();
        });
    }

    bool grouped(const std::shared_ptr<Part> &part) const
    {
        if (!part || !part->app || !part->session || !part->app->transport()
            || !part->app->transport()->supportsSharedTransport())
            return false;
        for (const auto &group : part->session->negotiatedGroupings())
            if (group.semantics == QStringLiteral("BUNDLE") && group.contents.contains(part->app->contentName()))
                return true;
        return false;
    }

    void closePart(const std::shared_ptr<Part> &part)
    {
        if (!part || part->closing || !part->app)
            return;
        part->closing = true;
        if (part->verified)
            part->app->acknowledgeReceived();
        else
            part->app->remove(J::Reason::Cancel, QStringLiteral("Media seek reprioritized"));
    }

    void plan()
    {
        if (stopped || scanning || !error.isEmpty() || !client)
            return;
        if (pending) {
            if (!pending->accepted)
                return;
            if (current && current != pending && !current->verified && !current->drained)
                return;
            if (current != pending)
                closePart(current);
            current = std::exchange(pending, {});
        }
        if (map.complete()) {
            // The final record can be committed before the negotiated range
            // checksum arrives. Finish through its verified receipt, not cancel.
            if (current && !current->verified)
                return;
            closePart(current);
            bool finished = true;
            for (const auto &part : parts)
                if (part->app && part->app->state() != J::State::Finished)
                    finished = false;
            if (finished) {
                timer->stop();
                QSet<J::Session *> sessions;
                for (const auto &part : parts)
                    if (part->session)
                        sessions.insert(part->session);
                for (auto *session : sessions)
                    if (session->state() < J::State::Finishing)
                        session->terminate(J::Reason::Success);
                current.reset();
                parts.clear();
            }
            return;
        }
        if (current && !current->accepted)
            return;
        auto gap = map.nextGap(priority);
        if (current && !current->verified) {
            if (!demandPending)
                return;
            demandPending = false;
            // Sequential reads and cached seeks that meet the current receive
            // head need no new content, even while that chunk is partial.
            if (!gap || (current->sink && gap->first == current->sink->nextChunk()))
                return;
            QSet<quint64> reserved;
            if (current->sink && current->sink->hasPartialChunk())
                reserved.insert(current->sink->nextChunk());
            gap = map.nextGap(priority, reserved);
        }
        if (!gap)
            gap = map.nextGap(0);
        if (!gap)
            return;
        if (current && !current->verified && current->app) {
            current->draining = true;
            if (current->sink && !current->sink->hasPartialChunk()) {
                current->drained = true;
                current->app->setReceivingPaused(true);
            }
        }
        pending      = std::make_shared<Part>();
        pending->gap = *gap;
        parts.append(pending);
        timer->start(timeout);
        if (grouped(current))
            addContent(pending);
        else
            startPublication(pending);
    }

    std::pair<quint64, quint64> range(const IrisMediaGapMap::Gap &gap) const
    {
        const auto begin = *MediaChunkWire::wireChunkOffset(parameters, gap.first);
        const auto end   = gap.first + gap.count == parameters.chunkCount()
            ? *MediaChunkWire::wireSize(parameters)
            : *MediaChunkWire::wireChunkOffset(parameters, gap.first + gap.count);
        return { begin, end - begin };
    }

    void configure(const std::shared_ptr<Part> &part, FT::Application *app)
    {
        part->app = app;
        app->setKeepTransportUntilReceipt();
        app->setReceiptDeferred();
        part->sink = new IrisMediaChunkSink(
            parameters, part->gap,
            [this, weak = std::weak_ptr<Part>(part)](quint64 index, const QByteArray &wire, const QByteArray &) {
                const auto part = weak.lock();
                if (!part || stopped || !error.isEmpty())
                    return IrisMediaChunkSink::Action::Error;
                if (!QDir().mkpath(directory))
                    return IrisMediaChunkSink::Action::Error;
                QSaveFile file(path(index));
                file.setDirectWriteFallback(false);
                if (!file.open(QIODevice::WriteOnly))
                    return IrisMediaChunkSink::Action::Error;
                file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                if (file.write(wire) != wire.size() || !file.commit())
                    return IrisMediaChunkSink::Action::Error;
                markAvailable(index);
                deliver(index, wire);
                timer->start(timeout);
                if (part->draining) {
                    part->drained = true;
                    if (part->app)
                        part->app->setReceivingPaused(true);
                    schedule();
                    return IrisMediaChunkSink::Action::Pause;
                }
                schedule();
                return IrisMediaChunkSink::Action::Continue;
            },
            app);
        const auto [offset, length] = range(part->gap);
        QObject::connect(app, &FT::Application::deviceRequested, q,
                         [this, part, offset, length](quint64 actualOffset, std::optional<quint64> actualLength) {
                             if (actualOffset != offset || !actualLength || *actualLength != length
                                 || !part->sink->isOpen()) {
                                 fail(QStringLiteral("Peer changed the requested media gap"));
                                 return;
                             }
                             transferring = true;
                             report();
                             part->app->setDevice(part->sink, false);
                         });
        QObject::connect(app, &FT::Application::progress, q, [this](quint64) {
            if (!stopped && error.isEmpty())
                timer->start(timeout);
        });
        QObject::connect(app, &FT::Application::payloadVerified, q, [this, part] {
            if (!part->sink || !part->sink->complete()) {
                if (!part->draining)
                    fail(QStringLiteral("Incomplete authenticated media gap"));
                return;
            }
            part->verified = true;
            schedule();
        });
        QObject::connect(app, &J::Application::stateChanged, q, [this, part](J::State state) {
            if (state == J::State::Active) {
                // The queued check observes committed content-accept grouping, not
                // the earlier Accepted state during description parsing.
                QTimer::singleShot(0, q, [this, part] {
                    if (!part->app || stopped || !error.isEmpty())
                        return;
                    if (part->shared && !grouped(part)) {
                        fail(QStringLiteral("Peer did not extend the media BUNDLE"));
                        return;
                    }
                    part->accepted = true;
                    schedule();
                });
            } else if (state == J::State::Finished) {
                if (!part->closing && !stopped && error.isEmpty())
                    fail(QStringLiteral("Media gap transfer failed: %1").arg(part->app->lastReason().text()));
                else
                    schedule();
            }
        });
    }

    void addContent(const std::shared_ptr<Part> &part)
    {
        part->session = current->session;
        auto *app     = static_cast<FT::Application *>(part->session->newContent(FT::NS, part->session->peerRole()));
        if (!app) {
            fail(QStringLiteral("Cannot add a media gap"));
            return;
        }
        auto file = current->app->file();
        file.setHashes({ XMPP::Hash(XMPP::Hash::Sha256) });
        const auto [offset, length] = range(part->gap);
        file.setRange(FT::Range(offset, length));
        app->setFile(file);
        const auto transport = part->session->newOutgoingTransport(current->app->transport()->pad()->ns());
        if (!transport || !app->setTransport(transport)) {
            fail(QStringLiteral("Cannot extend the media transport"));
            return;
        }
        part->shared = true;
        configure(part, app);
        qCDebug(lcIrisJingleMedia) << "Adding media gap:" << "sid=" << part->session->sid()
                                   << "first=" << part->gap.first << "count=" << part->gap.count;
        part->session->addContent(app);
    }

    void startPublication(const std::shared_ptr<Part> &part)
    {
        auto *manager = client->jingleManager();
        requestedSid.clear();
        incoming = QObject::connect(manager, &J::Manager::incomingSession, q, [this, part](J::Session *session) {
            const auto sid = request ? request->sid() : requestedSid;
            if (sid.isEmpty() || session->sid() != sid || !session->peer().compare(publication.from()))
                return;
            QObject::disconnect(incoming);
            if (session->contentList().size() != 1) {
                fail(QStringLiteral("Unexpected media publication contents"));
                return;
            }
            auto *base = session->contentList().first();
            if (!base || base->pad()->ns() != FT::NS || base->senders() != session->peerRole()) {
                session->terminate(J::Reason::SecurityError);
                fail(QStringLiteral("Invalid media publication offer"));
                return;
            }
            auto *app = static_cast<FT::Application *>(base);
            if (!app->file().size() || *app->file().size() != *MediaChunkWire::wireSize(parameters)) {
                session->terminate(J::Reason::SecurityError);
                fail(QStringLiteral("Media publication size changed"));
                return;
            }
            part->session               = session;
            auto accepted               = app->file();
            const auto [offset, length] = range(part->gap);
            accepted.setRange(FT::Range(offset, length));
            app->setAcceptFile(accepted);
            configure(part, app);
            QObject::connect(session, &J::Session::terminated, q, [this, weak = std::weak_ptr<Part>(part)] {
                const auto part = weak.lock();
                if (part && !part->closing && !stopped && error.isEmpty())
                    fail(QStringLiteral("Media session terminated"));
            });
            session->accept();
        });
        request  = manager->publicationManager()->requestPublishedSession(publication.from(), publication.id(), q);
        QObject::connect(request, &J::PublishedSessionRequest::finished, q,
                         [this, attempt = QPointer<J::PublishedSessionRequest>(request)] {
                             if (!attempt || request != attempt || stopped)
                                 return;
                             if (attempt->state() != J::PublishedSessionRequest::State::Succeeded)
                                 fail(QStringLiteral("Media publisher rejected the gap request"));
                             else
                                 requestedSid = attempt->sid();
                             if (request == attempt)
                                 request = nullptr;
                             attempt->deleteLater();
                         });
        request->start();
    }

    void fail(QString reason)
    {
        if (stopped || !error.isEmpty())
            return;
        error = std::move(reason);
        transferring = false;
        report();
        qCDebug(lcIrisJingleMedia) << "Media synchronization stopped:" << error;
        timer->stop();
        QObject::disconnect(incoming);
        if (request) {
            request->deleteLater();
            request = nullptr;
        }
        QSet<J::Session *> sessions;
        for (const auto &part : parts)
            if (part->session)
                sessions.insert(part->session);
        for (auto *session : sessions)
            if (session->state() < J::State::Finishing)
                session->terminate(J::Reason::FailedApplication, error);
        current.reset();
        pending.reset();
        parts.clear();
        const auto callbacks = std::exchange(waiters, {});
        const auto message   = error;
        QTimer::singleShot(0, q, [guard = QPointer<IrisMediaSync>(q), callbacks, message] {
            for (const auto &list : callbacks)
                for (const auto &callback : list) {
                    if (!guard)
                        return;
                    callback({}, message);
                }
        });
    }
};

IrisMediaSync::IrisMediaSync(XMPP::Client *client, J::JinglePub publication, MediaChunkWireParameters parameters,
                             QString directory, int timeoutMs, QObject *parent, MediaReference reference) :
    QObject(parent), d(std::make_unique<Impl>(this, client, std::move(publication), std::move(parameters),
                                              std::move(directory), timeoutMs, std::move(reference)))
{
}
IrisMediaSync::~IrisMediaSync() { cancel(); }
void IrisMediaSync::readWireChunk(quint64 index, MediaRangeService::Completion callback)
{
    d->read(index, std::move(callback));
}
void IrisMediaSync::prioritize(quint64 index)
{
    if (index >= d->parameters.chunkCount() || d->stopped)
        return;
    d->error.clear();
    d->priority      = index;
    d->demandPending = true;
    d->schedule();
}
void IrisMediaSync::setConnected(bool connected, XMPP::Client *client)
{
    if (connected && d->client == client && !d->stopped) {
        if (!d->error.isEmpty()) {
            d->error.clear();
            d->report();
        }
        d->schedule();
        return;
    }
    d->client = client;
    if (!connected && !d->stopped && (!d->parts.isEmpty() || d->request))
        d->fail(QStringLiteral("Media connection lost"));
    d->transferring = false;
    d->error.clear();
    d->report();
    if (connected && !d->stopped)
        d->schedule();
}
void IrisMediaSync::cancel()
{
    if (d->stopped)
        return;
    d->fail(QStringLiteral("Media synchronization cancelled"));
    d->stopped = true;
    d->report();
}
}
#endif
