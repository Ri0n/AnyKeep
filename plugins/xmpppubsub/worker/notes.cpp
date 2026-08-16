#include "xmppworker.h"

#include "private.h"

#include "privatenotespubsubitem.h"
#include "xmppkeysyncextension.h"
#include "xmppnotecodec.h"
#include "xmppomemostorage.h"

#include <QCoroFuture>
#include <QFutureInterface>
#include <QTimer>
#include <QXmppOmemoManager.h>
#include <QXmppPubSubManager.h>
#include <QXmppRosterManager.h>
#include <QXmppUtils.h>

#include <utility>
#include <variant>

namespace AnyKeep {

using namespace XmppWorkerPrivate;

void XmppWorker::cacheOwnOmemoBundle() { cacheOwnOmemoBundleTask(); }

void XmppWorker::scheduleOwnOmemoBundleRepair(uint32_t consumedPreKeyId)
{
    consumedOwnPreKeyIds_.insert(consumedPreKeyId);
    if (ownBundleRepairScheduled_)
        return;
    ownBundleRepairScheduled_ = true;
    // QXmpp publishes its incomplete in-memory bundle after returning from the pre-key storage callback.
    QTimer::singleShot(350, this, [this]() { repairOwnOmemoBundleAfterPreKeyUse(); });
}

void XmppWorker::repairOwnOmemoBundleAfterPreKeyUse(int attemptsRemaining)
{
    repairOwnOmemoBundleAfterPreKeyUseTask(attemptsRemaining);
}

void XmppWorker::probeAsync(StatusCallback callback) { ensureReadyTask().then(std::move(callback)); }

void XmppWorker::listNotesAsync(ListCallback callback) { listNotesTask().then(std::move(callback)); }

void XmppWorker::getNoteAsync(QString id, NoteCallback callback)
{
    getNoteTask(std::move(id)).then(std::move(callback));
}

void XmppWorker::saveNoteAsync(XmppRemoteNote note, NoteCallback callback)
{
    saveNoteTask(std::move(note)).then(std::move(callback));
}

void XmppWorker::updateNoteIndexAsync(XmppRemoteNote note, NoteCallback callback)
{
    updateNoteIndexTask(std::move(note)).then(std::move(callback));
}

void XmppWorker::deleteNoteAsync(QString id, StatusCallback callback)
{
    deleteNoteTask(std::move(id)).then(std::move(callback));
}

XmppDeviceInfo XmppWorker::ownOmemoDevice() const
{
    if (!omemoStorage_)
        return {};
    return { omemoStorage_->ownDeviceLabel(), omemoStorage_->ownDeviceId(), omemoStorage_->ownIdentityKey(),
             int(QXmpp::TrustLevel::Authenticated) };
}

void XmppWorker::ownOmemoDevicesAsync(DevicesCallback callback)
{
    ownOmemoDevicesTask().then([callback = std::move(callback)](auto result) mutable {
        callback(std::move(result.first), std::move(result.second));
    });
}

void XmppWorker::ownOmemoBundleValidAsync(StatusCallback callback)
{
    ownOmemoBundleValidTask().then(std::move(callback));
}

void XmppWorker::repairOwnOmemoDeviceAsync(StatusCallback callback)
{
    repairOwnOmemoDeviceTask().then(std::move(callback));
}

void XmppWorker::removeOwnOmemoDeviceAsync(quint32 deviceId, StatusCallback callback)
{
    removeOwnOmemoDeviceTask(deviceId).then(std::move(callback));
}

void XmppWorker::trustOwnOmemoDeviceAsync(QByteArray keyId, StatusCallback callback)
{
    trustOwnOmemoDeviceTask(std::move(keyId)).then(std::move(callback));
}

void XmppWorker::trustOwnOmemoDevicesAsync(QList<QByteArray> keyIds, StatusCallback callback)
{
    trustOwnOmemoDevicesTask(std::move(keyIds)).then(std::move(callback));
}

void XmppWorker::auditStorageKeysAsync(AuditCallback callback) { auditStorageKeysTask().then(std::move(callback)); }

void XmppWorker::rekeyStorageAsync(QList<QByteArray> keys, QByteArray canonicalKey, RekeyCallback callback)
{
    rekeyStorageTask(std::move(keys), std::move(canonicalKey)).then(std::move(callback));
}

void XmppWorker::scanObsoleteItemsAsync(CleanupCallback callback) { scanObsoleteItemsTask().then(std::move(callback)); }

void XmppWorker::deleteObsoleteItemsAsync(QStringList indexItemIds, QStringList contentItemIds,
                                          CleanupCallback callback)
{
    deleteObsoleteItemsTask(std::move(indexItemIds), std::move(contentItemIds)).then(std::move(callback));
}

void XmppWorker::approveKeySyncRequest(QString requestId) { approveKeySyncRequestTask(std::move(requestId)); }

void XmppWorker::rejectKeySyncRequest(QString requestId)
{
    pendingInboundKeyRequests_.remove(requestId);
    if (keySyncExtension_)
        keySyncExtension_->reject(requestId);
}

void XmppWorker::handleKeySyncRequest(const QString &requestId, const QString &from, const QByteArray &senderKey)
{
    handleKeySyncRequestTask(requestId, from, senderKey);
}

void XmppWorker::handleKeySyncTrustRequest(const QString &requestId, const QString &from, const QByteArray &senderKey)
{
    if (QXmppUtils::jidToBareJid(from) != QXmppUtils::jidToBareJid(config_.jid)) {
        keySyncExtension_->reject(requestId);
        return;
    }

    // This runs in reaction to an incoming stanza. Keep the complete lookup
    // asynchronous: nested event loops can receive a PubSub IQ response while
    // preventing QXmppTask's continuation from being dispatched.
    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto listTask = pubSub_->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                                  QStringLiteral("current"));
    listTask.then(this, [this, bareJid, requestId, senderKey](auto &&listResult) {
        if (const auto *requestError = std::get_if<QXmppError>(&listResult)) {
            keySyncExtension_->reject(requestId);
            emit backendError(errorText(*requestError));
            return;
        }

        struct LookupState {
            int     remaining { 0 };
            bool    matched { false };
            QString lastError;
        };
        const auto state   = std::make_shared<LookupState>();
        const auto ownId   = omemoStorage_->ownDeviceId();
        const auto devices = std::get<XmppOmemoDeviceListItem>(listResult).devices();
        for (const auto &listed : devices) {
            if (listed.id.toUInt() == ownId)
                continue;
            ++state->remaining;
            auto bundleTask = pubSub_->requestItem<XmppOmemoBundleItem>(
                bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), listed.id);
            bundleTask.then(this, [this, requestId, senderKey, listed, state](auto &&bundleResult) {
                if (state->matched)
                    return;
                if (const auto *requestError = std::get_if<QXmppError>(&bundleResult)) {
                    state->lastError = errorText(*requestError);
                } else {
                    const auto keyId = std::get<XmppOmemoBundleItem>(bundleResult).identityKey();
                    qInfo().noquote() << "Trust bootstrap OMEMO bundle: id=" << listed.id
                                      << "identity-key-size=" << keyId.size();
                    if (keyId == senderKey) {
                        state->matched = true;
                        finishKeySyncTrustRequest(requestId, senderKey);
                        return;
                    }
                }
                if (--state->remaining == 0) {
                    keySyncExtension_->reject(requestId);
                    emit backendError(state->lastError.isEmpty()
                                          ? QStringLiteral("Ignored a trust request from an unknown OMEMO device")
                                          : state->lastError);
                }
            });
        }
        if (state->remaining == 0) {
            keySyncExtension_->reject(requestId);
            emit backendError(QStringLiteral("Ignored a trust request because no other OMEMO device is published"));
        }
    });
}

void XmppWorker::finishKeySyncTrustRequest(const QString &requestId, const QByteArray &senderKey)
{
    auto trustTask = omemoManager_->trustLevel(QXmppUtils::jidToBareJid(config_.jid), senderKey);
    trustTask.then(this, [this, requestId, senderKey](QXmpp::TrustLevel trust) {
        if (trust == QXmpp::TrustLevel::ManuallyTrusted || trust == QXmpp::TrustLevel::Authenticated) {
            keySyncExtension_->replyTrustApproved(requestId);
            return;
        }
        pendingInboundKeyRequests_.insert(requestId, { senderKey, true });
        qInfo() << "Key-sync trust bootstrap needs user approval: id=" << requestId;
        emit keySyncTrustRequested(requestId, senderKey);
    });
}

QCoro::Task<XmppListResult> XmppWorker::listNotesTask()
{
    XmppListResult output;
    const auto     generation = clientGeneration_;
    const auto     ready      = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppListResult>();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }

    struct DecodeSummary {
        int     obsoleteItems { 0 };
        int     malformedItems { 0 };
        int     unsupportedItems { 0 };
        int     protectedUnreadableItems { 0 };
        int     keyMismatchItems { 0 };
        QString firstKeyMismatch;

        int skippedItems() const
        {
            return obsoleteItems + malformedItems + unsupportedItems + protectedUnreadableItems + keyMismatchItems;
        }
    } decodeSummary;
    const auto configuredKeyId = SecureEnvelope::keyId(config_.masterKey, KeyDerivationProfile::PrivateNotes);
    const auto decodeItems     = [this, &decodeSummary, &configuredKeyId](const auto &items, XmppListResult &result) {
        for (const auto &item : items) {
            if (!item.isValid()) {
                switch (item.parseFailure()) {
                case PrivateNotesPubSubItem::ParseFailure::ObsoleteFormat:
                    ++decodeSummary.obsoleteItems;
                    break;
                case PrivateNotesPubSubItem::ParseFailure::UnsupportedFormat:
                    ++decodeSummary.unsupportedItems;
                    break;
                case PrivateNotesPubSubItem::ParseFailure::Malformed:
                    ++decodeSummary.malformedItems;
                    break;
                case PrivateNotesPubSubItem::ParseFailure::None:
                    ++decodeSummary.protectedUnreadableItems;
                    break;
                }
                qWarning().noquote() << "Skipping unreadable XMPP index item" << item.id() << ':' << item.parseError();
                continue;
            }
            if (item.payload().keyId != configuredKeyId) {
                ++decodeSummary.keyMismatchItems;
                if (decodeSummary.firstKeyMismatch.isEmpty()) {
                    decodeSummary.firstKeyMismatch
                        = QStringLiteral("Encrypted private-note storage key mismatch (item %1, configured %2)")
                              .arg(QString::fromLatin1(item.payload().keyId.left(8).toHex()),
                                   QString::fromLatin1(configuredKeyId.left(8).toHex()));
                }
                qWarning().noquote() << "Skipping XMPP index item encrypted with another storage key" << item.id();
                continue;
            }
            auto note = XmppNoteCodec::decodeIndex(item.payload(), config_.masterKey, config_.indexNodeName());
            if (!note) {
                if (note.error.code == CryptoError::Unsupported)
                    ++decodeSummary.unsupportedItems;
                else
                    ++decodeSummary.protectedUnreadableItems;
                qWarning().noquote() << "Skipping unreadable XMPP index item" << item.id() << ':' << note.error.message;
                continue;
            }
            result.notes.append(std::move(note.value));
        }
    };

    auto idsResult = co_await pubSub_->requestOwnPepItemIds(config_.indexNodeName()).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppListResult>();
    if (std::holds_alternative<QVector<QString>>(idsResult)) {
        const auto   &ids       = std::get<QVector<QString>>(idsResult);
        constexpr int BatchSize = 50;
        for (int offset = 0; offset < ids.size(); offset += BatchSize) {
            QStringList batch;
            const int   end = qMin(offset + BatchSize, ids.size());
            for (int i = offset; i < end; ++i)
                batch.append(ids.at(i));

            auto result = co_await pubSub_
                              ->requestItems<PrivateNotesPubSubItem>(QXmppUtils::jidToBareJid(config_.jid),
                                                                     config_.indexNodeName(), batch)
                              .toFuture(this);
            if (generation != clientGeneration_)
                co_return configurationChangedResult<XmppListResult>();
            if (const auto *error = std::get_if<QXmppError>(&result)) {
                setXmppFailure(output, *error, errorText(*error));
                co_return output;
            }
            decodeItems(std::get<QXmppPubSubManager::Items<PrivateNotesPubSubItem>>(result).items, output);
        }
        if (output.notes.isEmpty() && decodeSummary.keyMismatchItems > 0) {
            output.error     = decodeSummary.firstKeyMismatch;
            output.errorKind = XmppErrorKind::Security;
            co_return output;
        }
        if (decodeSummary.skippedItems() > 0) {
            output.partial = true;
            qWarning() << "XMPP index refresh skipped" << decodeSummary.skippedItems()
                       << "unreadable item(s): obsolete=" << decodeSummary.obsoleteItems
                       << "malformed=" << decodeSummary.malformedItems
                       << "unsupported=" << decodeSummary.unsupportedItems
                       << "other-key=" << decodeSummary.keyMismatchItems
                       << "protected=" << decodeSummary.protectedUnreadableItems;
        }
        output.ok = true;
        co_return output;
    }

    // Compatibility fallback for servers without disco item IDs.
    auto result
        = co_await pubSub_
              ->requestItems<PrivateNotesPubSubItem>(QXmppUtils::jidToBareJid(config_.jid), config_.indexNodeName())
              .toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppListResult>();
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    const auto &items = std::get<QXmppPubSubManager::Items<PrivateNotesPubSubItem>>(result);
    output.partial    = items.continuation.has_value();
    decodeItems(items.items, output);
    if (output.notes.isEmpty() && decodeSummary.keyMismatchItems > 0) {
        output.error     = decodeSummary.firstKeyMismatch;
        output.errorKind = XmppErrorKind::Security;
        co_return output;
    }
    if (decodeSummary.skippedItems() > 0) {
        output.partial = true;
        qWarning() << "XMPP index refresh skipped" << decodeSummary.skippedItems()
                   << "unreadable item(s): obsolete=" << decodeSummary.obsoleteItems
                   << "malformed=" << decodeSummary.malformedItems << "unsupported=" << decodeSummary.unsupportedItems
                   << "other-key=" << decodeSummary.keyMismatchItems
                   << "protected=" << decodeSummary.protectedUnreadableItems;
    }
    output.ok = true;
    co_return output;
}

QCoro::Task<XmppNoteResult> XmppWorker::requestIndexTask(QString id, quint64 clientGeneration)
{
    XmppNoteResult output;
    auto           result
        = co_await pubSub_
              ->requestItem<PrivateNotesPubSubItem>(QXmppUtils::jidToBareJid(config_.jid), config_.indexNodeName(), id)
              .toFuture(this);
    if (clientGeneration != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        setXmppFailure(output, *error, errorText(*error));
        output.notFound = isItemNotFound(*error);
        co_return output;
    }
    const auto &item = std::get<PrivateNotesPubSubItem>(result);
    if (!item.isValid()) {
        output.error = item.parseError();
        co_return output;
    }
    auto index = XmppNoteCodec::decodeIndex(item.payload(), config_.masterKey, config_.indexNodeName());
    if (!index) {
        output.error = index.error.message;
        co_return output;
    }
    output.note = std::move(index.value);
    output.ok   = true;
    co_return output;
}

QCoro::Task<XmppNoteResult> XmppWorker::requestNoteTask(QString id, quint64 clientGeneration)
{
    // Content and index live in separate PubSub nodes and therefore cannot be
    // replaced atomically. A reader may briefly observe an old index together
    // with new content while another client is publishing a note.
    constexpr int snapshotAttempts          = 3;
    const auto    inconsistentSnapshotError = QStringLiteral("private-note content does not match its index revision");

    for (int attempt = 1; attempt <= snapshotAttempts; ++attempt) {
        XmppNoteResult output;
        auto           index = co_await requestIndexTask(id, clientGeneration);
        if (clientGeneration != clientGeneration_)
            co_return configurationChangedResult<XmppNoteResult>();
        if (!index.ok)
            co_return index;

        auto contentResult = co_await pubSub_
                                 ->requestItem<PrivateNotesPubSubItem>(QXmppUtils::jidToBareJid(config_.jid),
                                                                       config_.contentNodeName(), id)
                                 .toFuture(this);
        if (clientGeneration != clientGeneration_)
            co_return configurationChangedResult<XmppNoteResult>();
        if (const auto *error = std::get_if<QXmppError>(&contentResult)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        const auto &contentItem = std::get<PrivateNotesPubSubItem>(contentResult);
        if (!contentItem.isValid()) {
            output.error = contentItem.parseError();
            co_return output;
        }
        auto content = XmppNoteCodec::decodeContent(contentItem.payload(), config_.masterKey, config_.contentNodeName(),
                                                    index.note);
        if (!content) {
            if (content.error.message == inconsistentSnapshotError) {
                qInfo().noquote() << "Conflict trace: XMPP inconsistent snapshot note=" << id << "attempt=" << attempt
                                  << "index-revision=" << index.note.revision;
                if (attempt < snapshotAttempts)
                    continue;
                output.errorKind = XmppErrorKind::Transient;
            }
            output.error = content.error.message;
            co_return output;
        }
        output.note = std::move(content.value);
        output.ok   = true;
        co_return output;
    }

    Q_UNREACHABLE();
}

QCoro::Task<XmppNoteResult> XmppWorker::getNoteTask(QString id)
{
    const auto generation = clientGeneration_;
    const auto ready      = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (!ready.ok) {
        XmppNoteResult output;
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    co_return co_await requestNoteTask(std::move(id), generation);
}

QCoro::Task<XmppNoteResult> XmppWorker::publishNoteTask(XmppRemoteNote note, quint64 clientGeneration)
{
    XmppNoteResult output;
    note.parentRevision  = note.revision;
    note.revision        = newUuid();
    note.contentRevision = note.revision;
    note.originId        = config_.originId;
    qInfo().noquote() << "Conflict trace: XMPP publish note=" << note.id << "revision=" << note.revision
                      << "parent=" << note.parentRevision << "origin=" << note.originId
                      << "generation=" << clientGeneration;
    if (!note.preserveModified || !note.modified.isValid())
        note.modified = QDateTime::currentDateTimeUtc();
    note.format         = QStringLiteral("markdown");
    note.contentPresent = true;

    auto contentPayload = XmppNoteCodec::encodeContent(note, config_.masterKey, config_.contentNodeName());
    auto indexPayload   = XmppNoteCodec::encodeIndex(note, config_.masterKey, config_.indexNodeName());
    if (!contentPayload || !indexPayload) {
        output.error = !contentPayload ? contentPayload.error.message : indexPayload.error.message;
        co_return output;
    }

    auto published = co_await pubSub_
                         ->publishOwnPepItem(config_.contentNodeName(), PrivateNotesPubSubItem(contentPayload.value),
                                             privatePublishOptions())
                         .toFuture(this);
    if (clientGeneration != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (const auto *error = resultError(published)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    published = co_await pubSub_
                    ->publishOwnPepItem(config_.indexNodeName(), PrivateNotesPubSubItem(indexPayload.value),
                                        privatePublishOptions())
                    .toFuture(this);
    if (clientGeneration != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (const auto *error = resultError(published)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    output.note = std::move(note);
    output.ok   = true;
    co_return output;
}

QCoro::Task<XmppNoteResult> XmppWorker::saveNoteTask(XmppRemoteNote note)
{
    const auto generation = clientGeneration_;
    qInfo().noquote() << "Conflict trace: XMPP save begin note=" << note.id << "local-revision=" << note.revision
                      << "local-parent=" << note.parentRevision << "local-origin=" << note.originId
                      << "generation=" << generation;
    const auto ready = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (!ready.ok) {
        XmppNoteResult output;
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (note.id.isEmpty()) {
        note.id = newUuid();
    } else {
        auto server = co_await requestNoteTask(note.id, generation);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppNoteResult>();
        if (!server.ok)
            co_return server;
        qInfo().noquote() << "Conflict trace: XMPP revision check note=" << note.id << "local=" << note.revision
                          << "server=" << server.note.revision << "server-parent=" << server.note.parentRevision
                          << "server-origin=" << server.note.originId;
        if (server.note.revision != note.revision) {
            // A folder tree rename/reparent can publish an index-only update
            // while this editor still holds the preceding note revision. That
            // update has no body change, is a direct child of this local
            // revision, and carries this installation's origin ID. Rebase the
            // pending full save over exactly that case; every other revision
            // mismatch remains a real optimistic-concurrency conflict.
            const auto localContentRevision = note.contentRevision.isEmpty() ? note.revision : note.contentRevision;
            const auto serverContentRevision
                = server.note.contentRevision.isEmpty() ? server.note.revision : server.note.contentRevision;
            const bool ownIndexOnlyUpdate = server.note.originId == config_.originId
                && server.note.parentRevision == note.revision && serverContentRevision == localContentRevision;
            if (!ownIndexOnlyUpdate) {
                qInfo().noquote() << "Conflict trace: XMPP optimistic conflict note=" << note.id
                                  << "local=" << note.revision << "server=" << server.note.revision;
                XmppNoteResult conflict;
                conflict.conflict         = true;
                conflict.remoteOnConflict = std::move(server.note);
                conflict.error            = QStringLiteral(
                    "The note was modified on another XMPP resource; the local version was not published");
                co_return conflict;
            }

            qInfo().noquote() << "Conflict trace: XMPP rebasing local save over own index-only update note=" << note.id
                              << "local=" << note.revision << "server=" << server.note.revision;
            note.revision        = server.note.revision;
            note.contentRevision = serverContentRevision;
        }
    }
    co_return co_await publishNoteTask(std::move(note), generation);
}

QCoro::Task<XmppNoteResult> XmppWorker::updateNoteIndexTask(XmppRemoteNote note)
{
    const auto generation = clientGeneration_;
    const auto ready      = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (!ready.ok) {
        XmppNoteResult output;
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (note.id.isEmpty()) {
        XmppNoteResult output;
        output.error = QStringLiteral("A saved XMPP note ID is required for an index update");
        co_return output;
    }

    auto server = co_await requestIndexTask(note.id, generation);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (!server.ok)
        co_return server;
    if (server.note.revision != note.revision) {
        XmppNoteResult conflict;
        conflict.conflict         = true;
        conflict.remoteOnConflict = std::move(server.note);
        conflict.error = QStringLiteral("The note was modified on another XMPP resource; the folder was not published");
        co_return conflict;
    }

    auto updated           = std::move(server.note);
    updated.folderPath     = std::move(note.folderPath);
    updated.parentRevision = updated.revision;
    updated.revision       = newUuid();
    updated.originId       = config_.originId;
    updated.modified       = QDateTime::currentDateTimeUtc();
    updated.format         = QStringLiteral("markdown");
    updated.contentPresent = false;
    auto payload           = XmppNoteCodec::encodeIndex(updated, config_.masterKey, config_.indexNodeName());
    if (!payload) {
        XmppNoteResult output;
        output.error = payload.error.message;
        co_return output;
    }

    auto published = co_await pubSub_
                         ->publishOwnPepItem(config_.indexNodeName(), PrivateNotesPubSubItem(payload.value),
                                             privatePublishOptions())
                         .toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppNoteResult>();
    if (const auto *error = resultError(published)) {
        XmppNoteResult output;
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }

    XmppNoteResult output;
    output.note = std::move(updated);
    output.ok   = true;
    co_return output;
}

QCoro::Task<XmppStatusResult> XmppWorker::deleteNoteTask(QString id)
{
    const auto generation = clientGeneration_;
    auto       status     = co_await ensureReadyTask();
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppStatusResult>();
    if (!status.ok)
        co_return status;

    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       result  = co_await pubSub_->retractItem(bareJid, config_.indexNodeName(), id, true).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppStatusResult>();
    if (const auto *error = resultError(result); error && !isItemNotFound(*error))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    result = co_await pubSub_->retractItem(bareJid, config_.contentNodeName(), id, true).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppStatusResult>();
    if (const auto *error = resultError(result); error && !isItemNotFound(*error))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    co_return XmppStatusResult { true };
}

} // namespace AnyKeep
