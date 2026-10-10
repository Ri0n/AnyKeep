#ifndef ANYKEEP_IRISMEDIASYNC_H
#define ANYKEEP_IRISMEDIASYNC_H

#include "irischunkedmediasource.h"
#include "mediarangeservice.h"
#include <QIODevice>
#include <QSet>
#include <functional>
#include <iris/jingle-ft.h>
#include <memory>
#include <optional>

namespace XMPP {
class Client;
}
namespace AnyKeep {

// Chunk indices, never plaintext or transport byte offsets. Requested chunks
// remain separate from verified chunks until their records have been committed.
class IrisMediaGapMap {
public:
    struct Gap {
        quint64 first;
        quint64 count;
    };
    explicit IrisMediaGapMap(quint64 count) : count_(count) {}
    void markAvailable(quint64 index)
    {
        if (index < count_)
            available_.insert(index);
    }
    void               invalidate(quint64 index) { available_.remove(index); }
    bool               contains(quint64 index) const { return available_.contains(index); }
    bool               complete() const { return quint64(available_.size()) == count_; }
    std::optional<Gap> nextGap(quint64 start, const QSet<quint64> &reserved = {}) const
    {
        while (start < count_ && (contains(start) || reserved.contains(start)))
            ++start;
        if (start >= count_)
            return {};
        quint64 end = start + 1;
        while (end < count_ && !contains(end) && !reserved.contains(end))
            ++end;
        return Gap { start, end - start };
    }

private:
    quint64       count_;
    QSet<quint64> available_;
};

// One record of memory, even when a content spans a multi-gigabyte gap.
class IrisMediaChunkSink final : public QIODevice {
public:
    enum class Action { Continue, Pause, Error };
    using Commit = std::function<Action(quint64, const QByteArray &, const QByteArray &)>;
    IrisMediaChunkSink(MediaChunkWireParameters parameters, IrisMediaGapMap::Gap gap, Commit commit,
                       QObject *parent = nullptr);
    bool    isSequential() const override { return true; }
    quint64 nextChunk() const { return next_; }
    bool    hasPartialChunk() const { return !record_.isEmpty(); }
    bool    complete() const { return next_ == end_ && record_.isEmpty(); }

protected:
    qint64 readData(char *, qint64) override { return -1; }
    qint64 writeData(const char *data, qint64 size) override;

private:
    MediaChunkWireParameters parameters_;
    quint64                  next_, end_, limit_ = 0, received_ = 0;
    QByteArray               record_;
    Commit                   commit_;
    bool                     paused_ = false;
};

#ifdef IRIS_FT_DEFERRED_RECEIPTS
// One storage-owned synchronization task per immutable encrypted representation.
// Playback reads change its priority but do not own or terminate its download.
class IrisMediaSync final : public QObject {
public:
    IrisMediaSync(XMPP::Client *client, XMPP::Jingle::JinglePub publication, MediaChunkWireParameters parameters,
                  QString directory, int timeoutMs, QObject *parent = nullptr);
    ~IrisMediaSync() override;
    void readWireChunk(quint64 index, MediaRangeService::Completion callback);
    void prioritize(quint64 index);
    void cancel();

private:
    class Impl;
    std::unique_ptr<Impl> d;
};
#endif
}
#endif
