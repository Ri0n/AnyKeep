#include "xmppkeyresolutioncontroller.h"

#include <QSignalSpy>
#include <QTest>

using namespace AnyKeep;

class XmppKeyResolutionControllerTest : public QObject {
    Q_OBJECT

private slots:
    void completesRecovery();
    void requiresRecognizedDevice();
    void reportsPartialDeviceRefresh();
    void timesOutAndIgnoresLateDeviceResult();
    void removesPublishedDevice();
    void createsNewKeyOnlyWhenNoNotesExist();
    void startsFreshWithoutChangingUnreadableNotes();
    void firstInstallCanCreateKeyWithoutOmemoDiscovery();
    void firstInstallBackDiscardsGeneratedKey();
    void firstInstallKeyGenerationFailureCanRetry();
    void firstInstallCancelledDoesNotInstallKey();
    void existingLocalKeyCannotStartFresh();
    void freshKeyMismatchPolicyIsScopedToExplicitChoice();
    void reportsLiveProgressAndRejectsStaleUpdates();
    void failedSynchronizationKeepsUnknownNoteCount();
    void canonicalKeyConvergesAfterPeerMigration_data();
    void canonicalKeyConvergesAfterPeerMigration();
    void failedKeyInstallationCanRetry();
};

void XmppKeyResolutionControllerTest::completesRecovery()
{
    const QByteArray  deviceKey("device-key");
    const QByteArray  storageKey(32, 'k');
    QList<QByteArray> trustedKeys;
    QList<QByteArray> rekeyKeys;
    QByteArray        canonical;

    XmppDeviceInfo device;
    device.label      = QStringLiteral("Laptop");
    device.deviceId   = 42;
    device.keyId      = deviceKey;
    device.trustLevel = XmppTrustLevel::Undecided;

    XmppKeyResolutionController controller(
        true, { device }, {},
        [&trustedKeys](const QList<QByteArray> &keys, auto completion) {
            trustedKeys = keys;
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {},
        [storageKey](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 3;
            audit.candidates.append({ QStringLiteral("AnyKeep-desktop"), storageKey, QByteArray("key-id"), 3, false });
            completion(audit);
        },
        [&rekeyKeys, &canonical](const QList<QByteArray> &keys, const QByteArray &selected, auto completion) {
            rekeyKeys = keys;
            canonical = selected;
            XmppRekeyResult result;
            result.ok       = true;
            result.migrated = 3;
            result.total    = 3;
            completion(result);
        });

    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));

    controller.setDeviceSelected(0, true);
    controller.next();
    QCOMPARE(trustedKeys, QList<QByteArray> { deviceKey });
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::KeysPage));
    QCOMPARE(controller.selectedKeyIndex(), 0);

    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    QCOMPARE(controller.nextText(), QStringLiteral("Sync notes"));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QCOMPARE(controller.nextText(), QStringLiteral("Finish"));
    QVERIFY(controller.resultText().contains(QStringLiteral("Synchronization completed")));
    QCOMPARE(rekeyKeys, QList<QByteArray> { storageKey });
    QCOMPARE(canonical, storageKey);
    QVERIFY(controller.rekeyResult().ok);

    QSignalSpy finished(&controller, &XmppKeyResolutionController::finished);
    controller.next();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().first().toBool(), true);
}

void XmppKeyResolutionControllerTest::requiresRecognizedDevice()
{
    XmppDeviceInfo device;
    device.label      = QStringLiteral("Unknown");
    device.deviceId   = 7;
    device.keyId      = QByteArray("unknown-key");
    device.trustLevel = XmppTrustLevel::Undecided;

    bool                        trustCalled = false;
    XmppKeyResolutionController controller(
        true, { device }, {}, [&trustCalled](const QList<QByteArray> &, auto) { trustCalled = true; }, {}, [](auto) {},
        [](const QList<QByteArray> &, const QByteArray &, auto) {});

    controller.next();
    controller.next();
    QVERIFY(!trustCalled);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Select at least one")));
}

void XmppKeyResolutionControllerTest::reportsPartialDeviceRefresh()
{
    XmppDeviceInfo available;
    available.label      = QStringLiteral("Desktop");
    available.deviceId   = 42;
    available.keyId      = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    available.trustLevel = XmppTrustLevel::ManuallyTrusted;

    XmppDeviceInfo stale;
    stale.label    = QStringLiteral("Old device");
    stale.deviceId = 7;

    const QString               warning = QStringLiteral("Could not obtain the OMEMO fingerprint for 1 device(s)");
    XmppKeyResolutionController controller(true, { available, stale }, warning, {}, {}, {}, {});

    QCOMPARE(controller.devicesModel()->rowCount(), 2);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Found 2 OMEMO device(s)")));
    QVERIFY(controller.deviceStatus().contains(warning));
}

void XmppKeyResolutionControllerTest::timesOutAndIgnoresLateDeviceResult()
{
    XmppDeviceInfo device { QStringLiteral("Laptop"), 42, QByteArray("device-key"), XmppTrustLevel::Undecided };
    XmppKeyResolutionController::StatusCompletion lateCompletion;
    XmppKeyResolutionController                   controller(
        true, { device }, {},
        [&lateCompletion](const QList<QByteArray> &, auto completion) { lateCompletion = std::move(completion); }, {},
        [](auto) {}, [](const QList<QByteArray> &, const QByteArray &, auto) {}, 5);

    controller.next();
    controller.setDeviceSelected(0, true);
    controller.next();
    QVERIFY(controller.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 100);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Timed out")));
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));

    XmppStatusResult result;
    result.ok = true;
    lateCompletion(result);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));
}

void XmppKeyResolutionControllerTest::removesPublishedDevice()
{
    XmppDeviceInfo device { QStringLiteral("Old laptop"), 77, QByteArray("device-key"), XmppTrustLevel::Undecided };
    quint32        removedId = 0;
    XmppKeyResolutionController controller(
        true, { device }, {}, {},
        [&removedId](quint32 deviceId, auto completion) {
            removedId = deviceId;
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {}, {}, 1000);

    controller.next();
    controller.removeDevice(0);
    QCOMPARE(removedId, quint32(77));
    QCOMPARE(controller.devicesModel()->rowCount(), 0);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("was removed")));
}

void XmppKeyResolutionControllerTest::createsNewKeyOnlyWhenNoNotesExist()
{
    const QByteArray generatedKey(32, 'n');
    XmppDeviceInfo device { QStringLiteral("Desktop"), 42, QByteArray("device-key"), XmppTrustLevel::ManuallyTrusted };
    XmppKeyResolutionController controller(
        true, { device }, {},
        [](const QList<QByteArray> &, auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {},
        [](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok = true;
            completion(audit);
        },
        [](const QList<QByteArray> &, const QByteArray &, auto) {}, 1000,
        [generatedKey](auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result, generatedKey, QByteArray("generated-key-id"));
        });

    controller.next();
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::KeysPage));
    QVERIFY(controller.canCreateNewKey());
    controller.createNewKey();
    QCOMPARE(controller.canonicalKey(), generatedKey);
    QVERIFY(!controller.canCreateNewKey());
    QVERIFY(controller.keyStatus().contains(QStringLiteral("new storage key is ready")));
}

void XmppKeyResolutionControllerTest::startsFreshWithoutChangingUnreadableNotes()
{
    const QByteArray generatedKey(32, 'f');
    XmppDeviceInfo device { QStringLiteral("Desktop"), 42, QByteArray("device-key"), XmppTrustLevel::ManuallyTrusted };
    bool           rekeyCalled = false;
    XmppKeyResolutionController controller(
        true, { device }, {},
        [](const QList<QByteArray> &, auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {},
        [](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 2;
            completion(audit);
        },
        [&rekeyCalled](const QList<QByteArray> &, const QByteArray &, auto) { rekeyCalled = true; }, 1000,
        [generatedKey](auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result, generatedKey, QByteArray("fresh-key-id"));
        });

    controller.next();
    controller.next();
    QVERIFY(controller.canStartFresh());
    QVERIFY(!controller.canCreateNewKey());
    controller.startFresh();
    QVERIFY(controller.freshStart());
    QCOMPARE(controller.canonicalKey(), generatedKey);
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QVERIFY(controller.rekeyResult().ok);
    QVERIFY(!rekeyCalled);
}

void XmppKeyResolutionControllerTest::firstInstallCanCreateKeyWithoutOmemoDiscovery()
{
    const QByteArray            newKey(32, 'n');
    int                         auditCalls    = 0;
    int                         trustCalls    = 0;
    int                         rekeyCalls    = 0;
    int                         generateCalls = 0;
    XmppKeyResolutionController controller(
        true, {}, {}, [&trustCalls](const QList<QByteArray> &, auto) { ++trustCalls; }, {},
        [&auditCalls](auto) { ++auditCalls; },
        [&rekeyCalls](const QList<QByteArray> &, const QByteArray &, auto) { ++rekeyCalls; }, 1000,
        [&generateCalls, newKey](auto completion) {
            ++generateCalls;
            XmppStatusResult status;
            status.ok = true;
            completion(status, newKey, QByteArray("fresh-onboarding"));
        });

    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    QVERIFY(controller.canStartFresh());
    QVERIFY(!controller.canCreateNewKey());
    controller.startFresh();
    QCOMPARE(generateCalls, 1);
    QCOMPARE(auditCalls, 0);
    QCOMPARE(trustCalls, 0);
    QVERIFY(controller.freshStart());
    QCOMPARE(controller.canonicalKey(), newKey);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    QVERIFY(controller.summary().contains(QStringLiteral("if any")));
    QVERIFY(!controller.summary().contains(QStringLiteral("unchanged: 0")));

    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QVERIFY(controller.rekeyResult().ok);
    QCOMPARE(rekeyCalls, 0);
    QVERIFY(controller.resultText().contains(QStringLiteral("if any")));
    QSignalSpy finished(&controller, &XmppKeyResolutionController::finished);
    controller.next();
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().first().toBool());
}

void XmppKeyResolutionControllerTest::firstInstallBackDiscardsGeneratedKey()
{
    int                         generated = 0;
    XmppKeyResolutionController controller(true, {}, {}, {}, {}, {}, {}, 1000, [&generated](auto completion) {
        ++generated;
        XmppStatusResult status;
        status.ok = true;
        completion(status, QByteArray(32, 'x'), QByteArray("generated-id"));
    });
    controller.startFresh();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    QVERIFY(!controller.canonicalKey().isEmpty());

    controller.back();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    QVERIFY(controller.canonicalKey().isEmpty());
    QVERIFY(!controller.freshStart());
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));
    QVERIFY(controller.canonicalKey().isEmpty());
    QCOMPARE(generated, 1);
}

void XmppKeyResolutionControllerTest::firstInstallKeyGenerationFailureCanRetry()
{
    int                         attempts = 0;
    XmppKeyResolutionController controller(true, {}, {}, {}, {}, {}, {}, 1000, [&attempts](auto completion) {
        ++attempts;
        XmppStatusResult status;
        status.ok    = attempts > 1;
        status.error = status.ok ? QString() : QStringLiteral("Key generator unavailable");
        completion(status, status.ok ? QByteArray(32, 'k') : QByteArray(),
                   status.ok ? QByteArray("recovered-generator") : QByteArray());
    });
    controller.startFresh();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    QVERIFY(controller.keyStatus().contains(QStringLiteral("Key generator unavailable")));
    QVERIFY(controller.canStartFresh());
    controller.startFresh();
    QCOMPARE(attempts, 2);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
}

void XmppKeyResolutionControllerTest::firstInstallCancelledDoesNotInstallKey()
{
    int                         generateCalls = 0;
    XmppKeyResolutionController controller(true, {}, {}, {}, {}, {}, {}, 1000, [&generateCalls](auto completion) {
        ++generateCalls;
        XmppStatusResult status;
        status.ok = true;
        completion(status, QByteArray(32, 'k'), QByteArray("new-key"));
    });
    QSignalSpy                  finished(&controller, &XmppKeyResolutionController::finished);
    controller.startFresh();
    controller.cancel();
    QCOMPARE(generateCalls, 1);
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!controller.rekeyResult().ok);
}

void XmppKeyResolutionControllerTest::existingLocalKeyCannotStartFresh()
{
    int                         generated = 0;
    XmppKeyResolutionController controller(false, {}, {}, {}, {}, {}, {}, 1000,
                                           [&generated](auto completion) { ++generated; });
    QVERIFY(!controller.canStartFresh());
    controller.startFresh();
    QCOMPARE(generated, 0);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
}

void XmppKeyResolutionControllerTest::freshKeyMismatchPolicyIsScopedToExplicitChoice()
{
    XmppConfig config;
    // Foreign records require recovery even when other notes are readable.
    QVERIFY(config.foreignKeysRequireRecovery(3));
    QVERIFY(!config.foreignKeysRequireRecovery(0));

    // A user-selected fresh local storage preserves and ignores old indices,
    // instead of immediately reopening recovery after installing the key.
    config.allowForeignKeyIndices = true;
    QVERIFY(!config.foreignKeysRequireRecovery(3));
}

void XmppKeyResolutionControllerTest::reportsLiveProgressAndRejectsStaleUpdates()
{
    XmppDeviceInfo device;
    device.deviceId   = 42;
    device.keyId      = QByteArray("device");
    device.trustLevel = XmppTrustLevel::ManuallyTrusted;
    XmppKeyResolutionController::AuditCompletion auditCompletion;
    XmppKeyResolutionController::RekeyCompletion rekeyCompletion;
    XmppKeyResolutionController                  controller(
        true, { device }, {},
        [](const QList<QByteArray> &, auto completion) {
            XmppStatusResult status;
            status.ok = true;
            completion(status);
        },
        {}, [&](auto completion) { auditCompletion = std::move(completion); },
        [&](const QList<QByteArray> &, const QByteArray &, auto completion) {
            rekeyCompletion = std::move(completion);
        });
    controller.next();
    controller.next();
    QVERIFY(controller.busy());
    const auto auditProgress = controller.progressCallback();
    auditProgress({ QStringLiteral("Checking note keys"), 1, 3 });
    QCOMPARE(controller.syncSummary(), QStringLiteral("Checking note keys"));
    QCOMPARE(controller.syncCompleted(), 1);
    QCOMPARE(controller.syncTotal(), 3);

    XmppKeyAuditResult audit;
    audit.ok              = true;
    audit.totalIndexItems = 3;
    audit.candidates.append({ QStringLiteral("Desktop"), QByteArray(32, 'k'), QByteArray("key-id"), 3, false });
    auditCompletion(audit);
    controller.next();
    controller.next();
    QVERIFY(controller.busy());
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    const auto beforeStale = controller.syncEvents();
    auditProgress({ QStringLiteral("Late audit update"), 3, 3 });
    QCOMPARE(controller.syncEvents(), beforeStale);

    const auto progress = controller.progressCallback();
    QSignalSpy updates(&controller, &XmppKeyResolutionController::syncProgressChanged);
    progress({ QStringLiteral("Waiting for the server") });
    QCOMPARE(controller.syncCompleted(), -1);
    QCOMPARE(controller.syncTotal(), -1);
    for (int i = 0; i < 50; ++i)
        progress({ QStringLiteral("Processed note %1").arg(i), i, 50 });
    QCOMPARE(controller.syncEvents().size(), 40);
    QVERIFY(controller.syncEvents().last().endsWith(QStringLiteral("Processed note 49")));
    QCOMPARE(controller.syncCompleted(), 49);
    QCOMPARE(controller.syncTotal(), 50);
    QCOMPARE(updates.count(), 51);

    XmppRekeyResult done;
    done.ok        = true;
    done.total     = 3;
    done.migrated  = 1;
    done.unchanged = 2;
    rekeyCompletion(done);
    QVERIFY(!controller.busy());
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QCOMPARE(controller.syncCompleted(), 3);
    QCOMPARE(controller.syncTotal(), 3);
    QVERIFY(controller.resultText().contains(QStringLiteral("Verified without changes: 2")));
    QVERIFY(controller.resultText().contains(QStringLiteral("Re-encrypted: 1")));
    const auto finalEvents = controller.syncEvents();
    progress({ QStringLiteral("Late sync update"), 1, 50 });
    QCOMPARE(controller.syncEvents(), finalEvents);
    controller.abort();
    progress({ QStringLiteral("Update after abort") });
    QCOMPARE(controller.syncEvents(), finalEvents);
}

void XmppKeyResolutionControllerTest::failedSynchronizationKeepsUnknownNoteCount()
{
    XmppDeviceInfo device;
    device.deviceId   = 42;
    device.keyId      = QByteArray("device");
    device.trustLevel = XmppTrustLevel::ManuallyTrusted;
    XmppKeyResolutionController controller(
        false, { device }, {},
        [](const QList<QByteArray> &, auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {},
        [](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 3;
            audit.candidates.append({ QStringLiteral("Desktop"), QByteArray(32, 'k'), QByteArray("id"), 3, false });
            completion(audit);
        },
        [](const QList<QByteArray> &, const QByteArray &, auto completion) {
            XmppRekeyResult result;
            result.error = QStringLiteral("Connection lost before reading the note index");
            completion(result);
        });
    for (int page = 0; page < 4; ++page)
        controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QVERIFY(!controller.busy());
    QCOMPARE(controller.syncCompleted(), -1);
    QCOMPARE(controller.syncTotal(), -1);
    QVERIFY(controller.syncSummary().contains(QStringLiteral("Connection lost")));
}

void XmppKeyResolutionControllerTest::canonicalKeyConvergesAfterPeerMigration_data()
{
    QTest::addColumn<bool>("keepFirstKey");
    QTest::addColumn<bool>("sourceOnline");
    QTest::newRow("keep-first-key-online-peer") << true << true;
    QTest::newRow("keep-second-key-online-peer") << false << true;
    QTest::newRow("keep-first-key-source-offline") << true << false;
    QTest::newRow("keep-second-key-source-offline") << false << false;
}

void XmppKeyResolutionControllerTest::canonicalKeyConvergesAfterPeerMigration()
{
    QFETCH(bool, keepFirstKey);
    QFETCH(bool, sourceOnline);
    const QByteArray firstKey(32, 'a'), secondKey(32, 'b');
    const auto       canonical = keepFirstKey ? firstKey : secondKey;
    XmppDeviceInfo   device;
    device.deviceId           = 42;
    device.keyId              = QByteArrayLiteral("trusted-device");
    device.trustLevel         = XmppTrustLevel::ManuallyTrusted;
    auto       trust          = [](const QList<QByteArray> &, auto completion) { completion({ true }); };
    QByteArray installedFirst = firstKey;
    QByteArray pendingKey;
    XmppKeyResolutionController::StatusCompletion installCompletion;
    int                                           initialMigrations = 0;
    XmppKeyResolutionController                   initiator(
        false, { device }, {}, trust, {},
        [&](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 3;
            audit.candidates      = { { QStringLiteral("first"), firstKey, QByteArrayLiteral("id-a"), 2, true },
                                      { QStringLiteral("second"), secondKey, QByteArrayLiteral("id-b"), 1, false } };
            completion(audit);
        },
        [&](const QList<QByteArray> &keys, const QByteArray &selected, auto completion) {
            QCOMPARE(selected, canonical);
            QVERIFY(keys.contains(firstKey));
            QVERIFY(keys.contains(secondKey));
            XmppRekeyResult result;
            result.ok         = true;
            result.total      = 3;
            result.migrated   = keepFirstKey ? 1 : 2;
            result.unchanged  = result.total - result.migrated;
            initialMigrations = result.migrated;
            completion(result);
        });
    initiator.setKeyInstaller([&](QByteArray key, bool freshStart, auto completion) {
        QVERIFY(!freshStart);
        pendingKey        = key;
        installCompletion = std::move(completion);
    });
    QSignalSpy firstFinished(&initiator, &XmppKeyResolutionController::finished);
    initiator.next();
    initiator.next();
    QCOMPARE(initiator.selectedKeyIndex(), 0); // Largest key group is preferred.
    initiator.selectKey(keepFirstKey ? 0 : 1);
    initiator.next();
    initiator.next();
    QVERIFY(initialMigrations > 0);
    QVERIFY(initiator.rekeyResult().ok);
    initiator.next();
    QVERIFY(initiator.busy());
    QCOMPARE(firstFinished.size(), 0);
    QCOMPARE(installedFirst, firstKey); // No acceptance before durable installation.
    QCOMPARE(pendingKey, canonical);
    installedFirst = pendingKey; // Test keychain boundary; the source can now serve this key.
    installCompletion({ true });
    QCOMPARE(firstFinished.size(), 1);
    QVERIFY(firstFinished.first().first().toBool());

    // Another client still has the other key. After PEP invalidation (or a
    // later reconnect), the server index belongs entirely to the canonical key.
    QByteArray                  installedPeer     = keepFirstKey ? secondKey : firstKey;
    const auto                  oldPeerKey        = installedPeer;
    int                         peerInstallations = 0;
    XmppKeyResolutionController peer(
        false, { device }, {}, trust, {},
        [&](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 3;
            audit.candidates      = { { QStringLiteral("local"), oldPeerKey, QByteArrayLiteral("old-id"), 0, true },
                                      { QStringLiteral("source"), sourceOnline ? installedFirst : QByteArray {},
                                        QByteArrayLiteral("canonical-id"), 3, false } };
            completion(audit);
        },
        [&](const QList<QByteArray> &keys, const QByteArray &selected, auto completion) {
            XmppRekeyResult result;
            result.total = 3;
            if (sourceOnline) {
                QCOMPARE(selected, canonical);
                QVERIFY(keys.contains(canonical));
                result.ok        = true;
                result.unchanged = 3; // Receiving the canonical key causes no new publications.
            } else {
                QVERIFY(!keys.contains(canonical));
                result.error               = QStringLiteral("Some note keys are unavailable");
                result.inaccessibleNoteIds = { QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three") };
            }
            completion(result);
        });
    peer.setKeyInstaller([&](QByteArray key, bool freshStart, auto completion) {
        QVERIFY(!freshStart);
        ++peerInstallations;
        installedPeer = key;
        completion({ true });
    });
    QSignalSpy peerFinished(&peer, &XmppKeyResolutionController::finished);
    peer.next();
    peer.next();
    if (sourceOnline) {
        QCOMPARE(peer.selectedKeyIndex(), 1);
        QCOMPARE(peer.canonicalKey(), canonical);
    } else {
        peer.selectKey(1); // An offline/unknown key cannot be selected.
        QCOMPARE(peer.canonicalKey(), oldPeerKey);
    }
    peer.next();
    peer.next();
    peer.next();
    QCOMPARE(peerFinished.size(), 1);
    QCOMPARE(peerFinished.first().first().toBool(), sourceOnline);
    QCOMPARE(peerInstallations, sourceOnline ? 1 : 0);
    QCOMPARE(installedPeer, sourceOnline ? canonical : oldPeerKey);
    QCOMPARE(peer.rekeyResult().migrated, 0);
}

void XmppKeyResolutionControllerTest::failedKeyInstallationCanRetry()
{
    XmppDeviceInfo device;
    device.deviceId   = 42;
    device.keyId      = QByteArrayLiteral("trusted-device");
    device.trustLevel = XmppTrustLevel::ManuallyTrusted;
    XmppKeyResolutionController controller(
        false, { device }, {}, [](const QList<QByteArray> &, auto completion) { completion({ true }); }, {},
        [](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok         = true;
            audit.candidates = { { {}, QByteArray(32, 'k'), QByteArrayLiteral("key-id"), 1, false } };
            completion(audit);
        },
        [](const QList<QByteArray> &, const QByteArray &, auto completion) {
            XmppRekeyResult result;
            result.ok    = true;
            result.total = result.unchanged = 1;
            completion(result);
        });
    int attempts = 0;
    controller.setKeyInstaller([&](QByteArray, bool, auto completion) {
        XmppStatusResult result;
        result.ok    = ++attempts > 1;
        result.error = result.ok ? QString {} : QStringLiteral("Keychain unavailable");
        completion(result);
    });
    QSignalSpy finished(&controller, &XmppKeyResolutionController::finished);
    for (int i = 0; i < 5; ++i)
        controller.next();
    QCOMPARE(attempts, 1);
    QCOMPARE(finished.size(), 0);
    QVERIFY(!controller.completed());
    QVERIFY(!controller.busy());
    QVERIFY(controller.canGoNext());
    QVERIFY(controller.resultText().contains(QStringLiteral("Keychain unavailable")));
    controller.next();
    QCOMPARE(attempts, 2);
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().first().toBool());
}

QTEST_GUILESS_MAIN(XmppKeyResolutionControllerTest)

#include "xmppkeyresolutioncontroller_test.moc"
