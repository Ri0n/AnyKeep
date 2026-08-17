#include "xmppworker.h"

#include "private.h"

#include "privatenotespubsubitem.h"
#include "xmppkeysyncextension.h"
#include "xmppnotecodec.h"
#include "xmppomemostorage.h"

#include <QCoroFuture>
#include <QTimer>
#include <QUuid>
#include <QXmppClient.h>
#include <QXmppOmemoManager.h>
#include <QXmppPubSubManager.h>
#include <QXmppUtils.h>

#include <utility>
#include <variant>

namespace AnyKeep {

using namespace XmppWorkerPrivate;

QCoro::Task<XmppCleanupResult> XmppWorker::scanNodeForObsoleteItemsTask(QString                    nodeName,
                                                                        XmppEncryptedPayload::Kind expectedKind)
{
    XmppCleanupResult output;
    const auto        generation = clientGeneration_;
    auto              idsResult  = co_await pubSub_->requestOwnPepItemIds(nodeName).toFuture(this);
    if (generation != clientGeneration_)
        co_return configurationChangedResult<XmppCleanupResult>();
    if (const auto *error = std::get_if<QXmppError>(&idsResult)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }

    const auto    ids       = std::get<QVector<QString>>(idsResult);
    const auto    bareJid   = QXmppUtils::jidToBareJid(config_.jid);
    constexpr int BatchSize = 50;
    for (int offset = 0; offset < ids.size(); offset += BatchSize) {
        QStringList batch;
        for (int i = offset; i < qMin(offset + BatchSize, ids.size()); ++i)
            batch.append(ids.at(i));
        auto items = co_await pubSub_->requestItems<PrivateNotesPubSubItem>(bareJid, nodeName, batch).toFuture(this);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppCleanupResult>();
        if (const auto *error = std::get_if<QXmppError>(&items)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        for (const auto &item : std::get<QXmppPubSubManager::Items<PrivateNotesPubSubItem>>(items).items) {
            bool obsolete = false;
            if (!item.isValid()) {
                obsolete = item.isObsoleteOrMalformed();
            } else {
                const auto decodeError
                    = XmppNoteCodec::validatePayload(item.payload(), expectedKind, config_.masterKey, nodeName);
                if (!decodeError)
                    ++output.validItems;
                else if (decodeError.code == CryptoError::Corrupt)
                    obsolete = true;
                else
                    ++output.protectedUnreadableItems;
            }
            if (obsolete) {
                if (expectedKind == XmppEncryptedPayload::Index)
                    output.obsoleteIndexItemIds.append(item.id());
                else
                    output.obsoleteContentItemIds.append(item.id());
            } else if (!item.isValid()) {
                ++output.protectedUnreadableItems;
            }
        }
    }
    output.ok = true;
    co_return output;
}

QCoro::Task<XmppCleanupResult> XmppWorker::scanObsoleteItemsTask()
{
    XmppCleanupResult output;
    const auto        ready = co_await ensureReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }

    auto index = co_await scanNodeForObsoleteItemsTask(config_.indexNodeName(), XmppEncryptedPayload::Index);
    if (!index.ok)
        co_return index;
    auto content = co_await scanNodeForObsoleteItemsTask(config_.contentNodeName(), XmppEncryptedPayload::Content);
    if (!content.ok)
        co_return content;

    output.ok                       = true;
    output.obsoleteIndexItemIds     = std::move(index.obsoleteIndexItemIds);
    output.obsoleteContentItemIds   = std::move(content.obsoleteContentItemIds);
    output.protectedUnreadableItems = index.protectedUnreadableItems + content.protectedUnreadableItems;
    output.validItems               = index.validItems + content.validItems;
    co_return output;
}

QCoro::Task<XmppCleanupResult> XmppWorker::deleteObsoleteItemsTask(QStringList indexItemIds, QStringList contentItemIds)
{
    XmppCleanupResult output;
    const auto        ready = co_await ensureReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }

    struct Candidate {
        QString                    node;
        QString                    id;
        XmppEncryptedPayload::Kind kind;
    };
    QList<Candidate> candidates;
    candidates.reserve(indexItemIds.size() + contentItemIds.size());
    for (const auto &id : std::as_const(indexItemIds))
        candidates.append({ config_.indexNodeName(), id, XmppEncryptedPayload::Index });
    for (const auto &id : std::as_const(contentItemIds))
        candidates.append({ config_.contentNodeName(), id, XmppEncryptedPayload::Content });

    const auto bareJid    = QXmppUtils::jidToBareJid(config_.jid);
    const auto generation = clientGeneration_;
    for (const auto &candidate : std::as_const(candidates)) {
        auto current = co_await pubSub_->requestItem<PrivateNotesPubSubItem>(bareJid, candidate.node, candidate.id)
                           .toFuture(this);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppCleanupResult>();
        if (const auto *error = std::get_if<QXmppError>(&current)) {
            if (isItemNotFound(*error))
                continue;
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }

        const auto &item      = std::get<PrivateNotesPubSubItem>(current);
        bool        removable = !item.isValid() && item.isObsoleteOrMalformed();
        if (item.isValid()) {
            const auto decodeError
                = XmppNoteCodec::validatePayload(item.payload(), candidate.kind, config_.masterKey, candidate.node);
            removable = decodeError.code == CryptoError::Corrupt;
        }
        if (!removable) {
            ++output.protectedUnreadableItems;
            continue;
        }

        auto retracted = co_await pubSub_->retractItem(bareJid, candidate.node, candidate.id, true).toFuture(this);
        if (generation != clientGeneration_)
            co_return configurationChangedResult<XmppCleanupResult>();
        if (const auto *error = std::get_if<QXmppError>(&retracted)) {
            if (isItemNotFound(*error))
                continue;
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        ++output.removedItems;
    }
    output.ok = true;
    if (output.removedItems)
        emit remoteNodeInvalidated();
    co_return output;
}

QCoro::Task<XmppRekeyResult> XmppWorker::rekeyStorageTask(QList<QByteArray> keys, QByteArray canonicalKey)
{
    XmppRekeyResult output;
    const auto      ready = co_await ensureReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (canonicalKey.size() != SecureEnvelope::MasterKeySize) {
        output.error = QStringLiteral("The selected canonical XMPP storage key is invalid");
        co_return output;
    }
    QHash<QByteArray, QByteArray> keyring;
    for (const auto &key : keys) {
        if (key.size() == SecureEnvelope::MasterKeySize)
            keyring.insert(SecureEnvelope::keyId(key, KeyDerivationProfile::PrivateNotes), key);
    }
    keyring.insert(SecureEnvelope::keyId(canonicalKey, KeyDerivationProfile::PrivateNotes), canonicalKey);

    auto idsResult = co_await pubSub_->requestOwnPepItemIds(config_.indexNodeName()).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&idsResult)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    const auto ids     = std::get<QVector<QString>>(idsResult);
    output.total       = ids.size();
    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    for (const auto &id : ids) {
        auto indexResult = co_await pubSub_->requestItem<PrivateNotesPubSubItem>(bareJid, config_.indexNodeName(), id)
                               .toFuture(this);
        auto contentResult
            = co_await pubSub_->requestItem<PrivateNotesPubSubItem>(bareJid, config_.contentNodeName(), id)
                  .toFuture(this);
        const auto *indexError   = std::get_if<QXmppError>(&indexResult);
        const auto *contentError = std::get_if<QXmppError>(&contentResult);
        if (indexError || contentError) {
            const auto &error = indexError ? *indexError : *contentError;
            setXmppFailure(output, error, errorText(error));
            co_return output;
        }
        const auto &indexItem   = std::get<PrivateNotesPubSubItem>(indexResult);
        const auto &contentItem = std::get<PrivateNotesPubSubItem>(contentResult);
        if (!indexItem.isValid() || !contentItem.isValid()) {
            qWarning() << "Skipping unreadable XMPP note during rekey:" << id
                       << (!indexItem.isValid() ? indexItem.parseError() : contentItem.parseError());
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        const auto indexKey   = keyring.value(indexItem.payload().keyId);
        const auto contentKey = keyring.value(contentItem.payload().keyId);
        if (indexKey.isEmpty() || contentKey.isEmpty()) {
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        auto note = XmppNoteCodec::decodeIndex(indexItem.payload(), indexKey, config_.indexNodeName());
        if (!note) {
            qWarning() << "Skipping inaccessible XMPP note index during rekey:" << id << note.error.message;
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        auto content
            = XmppNoteCodec::decodeContent(contentItem.payload(), contentKey, config_.contentNodeName(), note.value);
        if (!content) {
            qWarning() << "Skipping inaccessible XMPP note content during rekey:" << id << content.error.message;
            output.inaccessibleNoteIds.append(id);
            continue;
        }
        note.value      = std::move(content.value);
        auto newContent = XmppNoteCodec::encodeContent(note.value, canonicalKey, config_.contentNodeName());
        auto newIndex   = XmppNoteCodec::encodeIndex(note.value, canonicalKey, config_.indexNodeName());
        if (!newContent || !newIndex) {
            output.error = !newContent ? newContent.error.message : newIndex.error.message;
            co_return output;
        }
        auto published = co_await pubSub_
                             ->publishOwnPepItem(config_.contentNodeName(), PrivateNotesPubSubItem(newContent.value),
                                                 privatePublishOptions())
                             .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&published)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        published = co_await pubSub_
                        ->publishOwnPepItem(config_.indexNodeName(), PrivateNotesPubSubItem(newIndex.value),
                                            privatePublishOptions())
                        .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&published)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        ++output.migrated;
    }
    output.ok = output.inaccessibleNoteIds.isEmpty();
    if (!output.ok)
        output.error = QStringLiteral("Some notes use storage keys that are not available");
    co_return output;
}

QCoro::Task<> XmppWorker::approveKeySyncRequestTask(QString requestId)
{
    const auto pending = pendingInboundKeyRequests_.take(requestId);
    if (pending.senderKey.isEmpty()) {
        qWarning() << "Key-sync approval has no pending request: id=" << requestId;
        co_return;
    }
    if (pending.trustBootstrap) {
        QMultiHash<QString, QByteArray> keys;
        keys.insert(QXmppUtils::jidToBareJid(config_.jid), pending.senderKey);
        co_await omemoManager_->setTrustLevel(keys, qxmppTrustLevel(XmppTrustLevel::ManuallyTrusted)).toFuture(this);
        keySyncExtension_->replyTrustApproved(requestId);
        co_return;
    }
    const auto trusted = co_await trustOwnOmemoDeviceTask(pending.senderKey);
    if (!trusted.ok) {
        emit backendError(trusted.error);
        co_return;
    }
    if (config_.masterKey.size() == SecureEnvelope::MasterKeySize)
        keySyncExtension_->replyWithKey(
            requestId, SecureEnvelope::encodeRecoveryKey(config_.masterKey, KeyDerivationProfile::PrivateNotes));
}

QCoro::Task<> XmppWorker::handleKeySyncRequestTask(QString requestId, QString from, QByteArray senderKey)
{
    qInfo().noquote() << "Handling key-sync request: id=" << requestId << "from=" << from
                      << "sender-key-size=" << senderKey.size();
    if (QXmppUtils::jidToBareJid(from) != QXmppUtils::jidToBareJid(config_.jid)) {
        keySyncExtension_->reject(requestId);
        co_return;
    }
    const auto trust
        = co_await omemoManager_->trustLevel(QXmppUtils::jidToBareJid(config_.jid), senderKey).toFuture(this);
    if (trust != QXmpp::TrustLevel::ManuallyTrusted && trust != QXmpp::TrustLevel::Authenticated) {
        auto [devices, error] = co_await ownOmemoDevicesTask();
        const bool ownDevice  = std::any_of(devices.cbegin(), devices.cend(),
                                            [&senderKey](const auto &device) { return device.keyId == senderKey; });
        if (!ownDevice) {
            keySyncExtension_->reject(requestId);
            emit backendError(
                error.isEmpty() ? QStringLiteral("Ignored a storage-key request from an unknown OMEMO device") : error);
            co_return;
        }
        pendingInboundKeyRequests_.insert(requestId, { senderKey, false });
        emit keySyncTrustRequested(requestId, senderKey);
        co_return;
    }
    if (config_.masterKey.size() != SecureEnvelope::MasterKeySize) {
        keySyncExtension_->reject(requestId);
        co_return;
    }
    keySyncExtension_->replyWithKey(
        requestId, SecureEnvelope::encodeRecoveryKey(config_.masterKey, KeyDerivationProfile::PrivateNotes));
}

QCoro::Task<> XmppWorker::cacheOwnOmemoBundleTask()
{
    if (!pubSub_ || !omemoStorage_ || !omemoStorage_->ownDeviceId())
        co_return;
    const auto deviceId = omemoStorage_->ownDeviceId();
    auto       result
        = co_await pubSub_
              ->requestItem<XmppOmemoBundleItem>(QXmppUtils::jidToBareJid(config_.jid),
                                                 QStringLiteral("urn:xmpp:omemo:2:bundles"), QString::number(deviceId))
              .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        qWarning().noquote() << "Could not cache own OMEMO bundle:" << errorText(*error);
        co_return;
    }
    auto bundle = std::get<XmppOmemoBundleItem>(std::move(result));
    if (!omemoStorage_ || deviceId != omemoStorage_->ownDeviceId()
        || bundle.identityKey() != omemoStorage_->ownIdentityKey()) {
        qWarning() << "Own OMEMO bundle was not cached because its identity key is invalid";
        co_return;
    }
    cachedOwnOmemoBundle_ = std::move(bundle);
}

QCoro::Task<> XmppWorker::repairOwnOmemoBundleAfterPreKeyUseTask(int attemptsRemaining)
{
    ownBundleRepairScheduled_ = false;
    if (!cachedOwnOmemoBundle_ || !pubSub_ || !client_ || !client_->isConnected())
        co_return;
    const auto bareJid  = QXmppUtils::jidToBareJid(config_.jid);
    const auto deviceId = omemoStorage_->ownDeviceId();
    auto       result   = co_await pubSub_
                              ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                                 QString::number(deviceId))
                              .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&result)) {
        qWarning().noquote() << "Could not inspect own OMEMO bundle after pre-key use:" << errorText(*error);
        co_return;
    }
    auto published = std::get<XmppOmemoBundleItem>(std::move(result));
    if (published.identityKey() == omemoStorage_->ownIdentityKey()) {
        bool containsConsumed = false;
        for (const auto id : consumedOwnPreKeyIds_)
            containsConsumed |= published.publicPreKeys().contains(id);
        if (containsConsumed && attemptsRemaining > 0) {
            ownBundleRepairScheduled_ = true;
            QTimer::singleShot(
                250, this, [this, attemptsRemaining]() { repairOwnOmemoBundleAfterPreKeyUse(attemptsRemaining - 1); });
            co_return;
        }
        cachedOwnOmemoBundle_ = std::move(published);
        consumedOwnPreKeyIds_.clear();
        co_return;
    }
    if (!published.identityKey().isEmpty()) {
        qWarning() << "Refusing to repair own OMEMO bundle with a mismatching identity key";
        co_return;
    }
    auto repaired = published.repairedFrom(*cachedOwnOmemoBundle_, consumedOwnPreKeyIds_);
    auto outcome
        = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), repaired).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&outcome)) {
        qWarning().noquote() << "Could not repair QXmpp's incomplete own OMEMO bundle:" << errorText(*error);
        co_return;
    }
    cachedOwnOmemoBundle_ = std::move(repaired);
    consumedOwnPreKeyIds_.clear();
}

QString XmppWorker::newUuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString XmppWorker::errorText(const QXmppError &error)
{
    return error.description.isEmpty() ? QStringLiteral("Unknown XMPP error") : error.description;
}

} // namespace AnyKeep
