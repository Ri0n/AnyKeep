#include "xmppworker.h"

#include "private.h"

#include "privatenotespubsubitem.h"
#include "xmppkeysyncextension.h"
#include "xmppnotecodec.h"
#include "xmppomemostorage.h"
#include "xmpppepextension.h"

#include <QCoroFuture>
#include <QXmppDiscoveryManager.h>
#include <QXmppOmemoManager.h>
#include <QXmppPubSubManager.h>
#include <QXmppRosterManager.h>
#include <QXmppTrustStorage.h>
#include <QXmppUtils.h>

#include <algorithm>
#include <variant>

namespace AnyKeep {

using namespace XmppWorkerPrivate;

QCoro::Task<std::pair<QList<XmppDeviceInfo>, QString>> XmppWorker::ownOmemoDevicesTask()
{
    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return std::make_pair(QList<XmppDeviceInfo> {}, ready.error);

    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       list    = co_await pubSub_
                             ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                                    QStringLiteral("current"))
                             .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return std::make_pair(QList<XmppDeviceInfo> {}, errorText(*error));

    const auto            ownDeviceId = omemoStorage_->ownDeviceId();
    const auto            ownKey      = omemoStorage_->ownIdentityKey();
    QList<XmppDeviceInfo> devices;
    int                   missingFingerprints = 0;
    for (const auto &listed : std::get<XmppOmemoDeviceListItem>(list).devices()) {
        if (listed.id.toUInt() == ownDeviceId)
            continue;
        auto bundle
            = co_await pubSub_
                  ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), listed.id)
                  .toFuture(this);
        QByteArray keyId;
        if (const auto *error = std::get_if<QXmppError>(&bundle)) {
            qWarning().noquote() << "Could not fetch OMEMO bundle: id=" << listed.id << "label=" << listed.label
                                 << "error=" << errorText(*error);
        } else {
            keyId = std::get<XmppOmemoBundleItem>(bundle).identityKey();
        }
        if (!keyId.isEmpty() && keyId == ownKey)
            continue;
        // Keep the client-published label separate from the numeric OMEMO ID.
        // AnyKeep uses its XMPP resource as the label, but that is not a protocol guarantee.
        const auto label = listed.label.isEmpty() ? QStringLiteral("Unnamed device") : listed.label;
        if (keyId.isEmpty()) {
            ++missingFingerprints;
            devices.append({ label, listed.id.toUInt(), {}, int(QXmpp::TrustLevel::Undecided) });
            continue;
        }
        const auto trust = co_await omemoManager_->trustLevel(bareJid, keyId).toFuture(this);
        devices.append({ label, listed.id.toUInt(), keyId, int(trust) });
    }
    const auto error = missingFingerprints
        ? QStringLiteral("Could not obtain the OMEMO fingerprint for %1 device(s)").arg(missingFingerprints)
        : QString {};
    co_return std::make_pair(std::move(devices), error);
}

QCoro::Task<XmppStatusResult> XmppWorker::ownOmemoBundleValidTask()
{
    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return ready;
    const auto own = ownOmemoDevice();
    if (!own.deviceId || own.keyId.isEmpty())
        co_return XmppStatusResult { false, false, false, QStringLiteral("The local OMEMO device is not initialized") };
    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       bundle  = co_await pubSub_
                             ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                                QString::number(own.deviceId))
                             .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&bundle))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    const auto publishedKey = std::get<XmppOmemoBundleItem>(bundle).identityKey();
    if (publishedKey != own.keyId) {
        co_return XmppStatusResult { false, false, false,
                                     publishedKey.isEmpty()
                                         ? QStringLiteral("The published OMEMO bundle has no identity key")
                                         : QStringLiteral("The published OMEMO identity key does not match") };
    }
    auto list = co_await pubSub_
                    ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                           QStringLiteral("current"))
                    .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    const auto &devices   = std::get<XmppOmemoDeviceListItem>(list).devices();
    const bool  announced = std::any_of(devices.cbegin(), devices.cend(),
                                        [&own](const auto &device) { return device.id.toUInt() == own.deviceId; });
    if (!announced)
        co_return XmppStatusResult {
            false, false, false, QStringLiteral("The local OMEMO device is missing from the published device list")
        };
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::trustOwnOmemoDeviceTask(QByteArray keyId)
{
    if (keyId.isEmpty())
        co_return XmppStatusResult { false, false, false, QStringLiteral("No OMEMO device was selected") };
    auto [devices, error] = co_await ownOmemoDevicesTask();
    const bool belongsToSelf
        = std::any_of(devices.cbegin(), devices.cend(), [&keyId](const auto &device) { return device.keyId == keyId; });
    if (!belongsToSelf)
        co_return XmppStatusResult { false, false, false,
                                     error.isEmpty() ? QStringLiteral("The OMEMO key does not belong to an own device")
                                                     : error };
    QMultiHash<QString, QByteArray> keys;
    keys.insert(QXmppUtils::jidToBareJid(config_.jid), keyId);
    co_await omemoManager_->setTrustLevel(keys, QXmpp::TrustLevel::ManuallyTrusted).toFuture(this);
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::trustOwnOmemoDevicesTask(QList<QByteArray> keyIds)
{
    for (const auto &keyId : keyIds) {
        const auto result = co_await trustOwnOmemoDeviceTask(keyId);
        if (!result.ok)
            co_return result;
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::repairOwnOmemoDeviceTask()
{
    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return ready;

    const auto bareJid      = QXmppUtils::jidToBareJid(config_.jid);
    const auto oldDeviceId  = omemoStorage_->ownDeviceId();
    const auto oldDeviceKey = omemoStorage_->ownIdentityKey();
    auto       oldBundle    = co_await pubSub_
                                  ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                                     QString::number(oldDeviceId))
                                  .toFuture(this);
    const bool bundleValid  = std::holds_alternative<XmppOmemoBundleItem>(oldBundle)
        && std::get<XmppOmemoBundleItem>(oldBundle).identityKey() == oldDeviceKey;

    auto list = co_await pubSub_
                    ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                           QStringLiteral("current"))
                    .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    auto item    = std::get<XmppOmemoDeviceListItem>(list);
    auto devices = item.devices();

    if (bundleValid) {
        if (std::none_of(devices.cbegin(), devices.cend(),
                         [oldDeviceId](const auto &device) { return device.id.toUInt() == oldDeviceId; })) {
            devices.append({ config_.resource, QString::number(oldDeviceId), {} });
            item.setDevices(std::move(devices));
            auto published = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"), item)
                                 .toFuture(this);
            if (const auto *error = std::get_if<QXmppError>(&published))
                co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
        }
        co_return XmppStatusResult { true };
    }

    co_await omemoManager_->resetOwnDeviceLocally().toFuture(this);
    omemoReady_      = false;
    const bool setup = co_await omemoManager_->setUp(config_.resource).toFuture(this);
    if (!setup)
        co_return XmppStatusResult { false, false, false,
                                     QStringLiteral("QXmpp could not create a replacement OMEMO device") };
    omemoReady_ = true;
    devices.removeIf([oldDeviceId](const auto &device) { return device.id.toUInt() == oldDeviceId; });
    const auto newDeviceId = omemoStorage_->ownDeviceId();
    if (std::none_of(devices.cbegin(), devices.cend(),
                     [newDeviceId](const auto &device) { return device.id.toUInt() == newDeviceId; }))
        devices.append({ config_.resource, QString::number(newDeviceId), {} });
    item.setDevices(std::move(devices));
    auto published
        = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"), item).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&published))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };
    if (oldDeviceId) {
        auto retracted
            = co_await pubSub_
                  ->retractItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), QString::number(oldDeviceId))
                  .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&retracted))
            qWarning().noquote() << "Could not retract old OMEMO bundle" << oldDeviceId << errorText(*error);
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<XmppStatusResult> XmppWorker::removeOwnOmemoDeviceTask(quint32 deviceId)
{
    if (!deviceId)
        co_return XmppStatusResult { false, false, false, QStringLiteral("Invalid OMEMO device ID") };

    auto ready = co_await ensureOmemoReadyTask();
    if (!ready.ok)
        co_return ready;
    if (deviceId == omemoStorage_->ownDeviceId())
        co_return XmppStatusResult { false, false, false,
                                     QStringLiteral("The current OMEMO device cannot be removed") };

    const auto bareJid = QXmppUtils::jidToBareJid(config_.jid);
    auto       list    = co_await pubSub_
                             ->requestItem<XmppOmemoDeviceListItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"),
                                                                    QStringLiteral("current"))
                             .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&list))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };

    auto       item    = std::get<XmppOmemoDeviceListItem>(list);
    auto       devices = item.devices();
    const auto oldSize = devices.size();
    devices.removeIf([deviceId](const auto &device) { return device.id.toUInt() == deviceId; });
    if (devices.size() == oldSize)
        co_return XmppStatusResult { false, false, true, QStringLiteral("The OMEMO device is no longer published") };

    QByteArray removedKey;
    auto       bundle = co_await pubSub_
                            ->requestItem<XmppOmemoBundleItem>(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"),
                                                               QString::number(deviceId))
                            .toFuture(this);
    if (const auto *publishedBundle = std::get_if<XmppOmemoBundleItem>(&bundle))
        removedKey = publishedBundle->identityKey();

    item.setDevices(std::move(devices));
    auto published
        = co_await pubSub_->publishItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:devices"), item).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&published))
        co_return XmppStatusResult { false, false, false, errorText(*error), {}, classifyXmppError(*error) };

    co_await omemoStorage_->removeDevice(bareJid, deviceId).toFuture(this);
    if (trustStorage_ && !removedKey.isEmpty()) {
        co_await trustStorage_->removeKeys(QStringLiteral("urn:xmpp:omemo:2"), QList<QByteArray> { removedKey })
            .toFuture(this);
    }
    auto retracted
        = co_await pubSub_->retractItem(bareJid, QStringLiteral("urn:xmpp:omemo:2:bundles"), QString::number(deviceId))
              .toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&retracted)) {
        co_return XmppStatusResult {
            true, false, false,
            QStringLiteral("Device removed from the OMEMO list, but its unreferenced bundle could not be deleted: %1")
                .arg(errorText(*error))
        };
    }
    co_return XmppStatusResult { true };
}

QCoro::Task<std::pair<QStringList, QString>> XmppWorker::onlinePrivateNotesResourcesTask()
{
    if (!roster_ || !discovery_)
        co_return std::make_pair(QStringList {}, QStringLiteral("XMPP resource discovery is unavailable"));
    const auto  bareJid     = QXmppUtils::jidToBareJid(config_.jid);
    const auto  ownResource = client_->configuration().resource();
    QStringList resources;
    QStringList failures;
    for (const auto &resource : roster_->getResources(bareJid)) {
        if (resource.isEmpty() || resource == ownResource)
            continue;
        const auto fullJid = bareJid + QLatin1Char('/') + resource;
#if QXMPP_VERSION >= QT_VERSION_CHECK(1, 12, 0)
        auto info = co_await discovery_->info(fullJid).toFuture(this);
#else
        auto info = co_await discovery_->requestDiscoInfo(fullJid).toFuture(this);
#endif
        if (const auto *error = std::get_if<QXmppError>(&info)) {
            failures.append(QStringLiteral("%1: %2").arg(fullJid, errorText(*error)));
            continue;
        }
        if (std::get<0>(info).features().contains(XmppKeySyncExtension::feature))
            resources.append(resource);
    }
    co_return std::make_pair(std::move(resources), failures.join(QLatin1Char('\n')));
}

QCoro::Task<XmppKeyAuditResult> XmppWorker::auditStorageKeysTask()
{
    XmppKeyAuditResult output;
    auto               ready = co_await ensureOmemoReadyTask();
    if (!ready.ok) {
        output.error     = ready.error;
        output.errorKind = ready.errorKind;
        co_return output;
    }
    if (client_->encryptionExtension() != omemoManager_ || !keySyncExtension_) {
        output.error = QStringLiteral("OMEMO key synchronization is unavailable");
        co_return output;
    }
    if (config_.masterKey.size() == SecureEnvelope::MasterKeySize)
        output.candidates.append({ client_->configuration().resource(), config_.masterKey,
                                   SecureEnvelope::keyId(config_.masterKey, KeyDerivationProfile::PrivateNotes), 0,
                                   true });

    const auto bareJid               = QXmppUtils::jidToBareJid(config_.jid);
    auto [resources, discoveryError] = co_await onlinePrivateNotesResourcesTask();
    QStringList errors;
    if (!discoveryError.isEmpty())
        errors.append(discoveryError);
    co_await omemoStorage_->resetAllSessions().toFuture(this);

    for (const auto &name : resources) {
        const auto resource = bareJid + QLatin1Char('/') + name;
        const auto trustId  = newUuid();
        auto       trustResult
            = co_await client_
                  ->sendIq(keySyncExtension_->makeTrustRequest(resource, trustId, omemoStorage_->ownIdentityKey()), {})
                  .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&trustResult)) {
            errors.append(QStringLiteral("%1: OMEMO trust bootstrap failed: %2").arg(resource, errorText(*error)));
            continue;
        }
        if (!XmppKeySyncExtension::isTrustApproved(std::get<QDomElement>(trustResult), trustId)) {
            errors.append(QStringLiteral("%1: invalid OMEMO trust bootstrap response").arg(resource));
            continue;
        }

        auto requestId = newUuid();
        auto result
            = co_await client_->sendSensitiveIq(keySyncExtension_->makeRequest(resource, requestId), {}).toFuture(this);
        if (std::holds_alternative<QXmppError>(result)) {
            requestId = newUuid();
            result    = co_await client_->sendSensitiveIq(keySyncExtension_->makeRequest(resource, requestId), {})
                            .toFuture(this);
        }
        if (const auto *error = std::get_if<QXmppError>(&result)) {
            errors.append(QStringLiteral("%1: %2").arg(resource, errorText(*error)));
            continue;
        }
        const auto encoded = XmppKeySyncExtension::responseRecoveryKey(std::get<QDomElement>(result), requestId);
        auto       key     = SecureEnvelope::decodeRecoveryKey(encoded, KeyDerivationProfile::PrivateNotes);
        if (!key) {
            errors.append(QStringLiteral("%1: invalid storage key response").arg(resource));
            continue;
        }
        const auto keyId    = SecureEnvelope::keyId(key.value, KeyDerivationProfile::PrivateNotes);
        auto       existing = std::find_if(output.candidates.begin(), output.candidates.end(),
                                           [&keyId](const auto &candidate) { return candidate.keyId == keyId; });
        if (existing == output.candidates.end())
            output.candidates.append({ resource, key.value, keyId, 0, false });
        else if (!existing->resource.split(QStringLiteral(", ")).contains(resource))
            existing->resource += QStringLiteral(", ") + resource;
    }

    auto idsResult = co_await pubSub_->requestOwnPepItemIds(config_.indexNodeName()).toFuture(this);
    if (const auto *error = std::get_if<QXmppError>(&idsResult)) {
        setXmppFailure(output, *error, errorText(*error));
        co_return output;
    }
    const auto ids          = std::get<QVector<QString>>(idsResult);
    output.totalIndexItems  = ids.size();
    constexpr int BatchSize = 50;
    for (int offset = 0; offset < ids.size(); offset += BatchSize) {
        QStringList batch;
        for (int i = offset; i < qMin(offset + BatchSize, ids.size()); ++i)
            batch.append(ids.at(i));
        auto items = co_await pubSub_->requestItems<PrivateNotesPubSubItem>(bareJid, config_.indexNodeName(), batch)
                         .toFuture(this);
        if (const auto *error = std::get_if<QXmppError>(&items)) {
            setXmppFailure(output, *error, errorText(*error));
            co_return output;
        }
        for (const auto &item : std::get<QXmppPubSubManager::Items<PrivateNotesPubSubItem>>(items).items) {
            if (!item.isValid()) {
                qWarning().noquote() << "Skipping unreadable XMPP index item during storage-key audit" << item.id()
                                     << ':' << item.parseError();
                continue;
            }
            const auto keyId     = item.payload().keyId;
            auto       candidate = std::find_if(output.candidates.begin(), output.candidates.end(),
                                                [&keyId](const auto &entry) { return entry.keyId == keyId; });
            if (candidate == output.candidates.end())
                output.candidates.append({ {}, {}, keyId, 1, false });
            else
                ++candidate->indexItemCount;
        }
    }
    output.ok = true;
    if (!errors.isEmpty())
        output.error = QStringLiteral("Some private-note resources failed:\n%1").arg(errors.join('\n'));
    co_return output;
}

} // namespace AnyKeep
