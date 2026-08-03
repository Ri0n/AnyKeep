#include "xmppomemostorage.h"

#include "secureenvelope.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace AnyKeep;

class XmppOmemoStorageTest : public QObject {
    Q_OBJECT

private slots:
    void persistsEncryptedState();
    void clearsPeerSessionsWithoutResettingOwnDevice();
    void resetsSessionsWithoutRemovingFingerprints();
    void preservesIdentityOnPartialOwnDeviceUpdate();
    void rejectsWrongLocalKey();
};

void XmppOmemoStorageTest::persistsEncryptedState()
{
    QTemporaryDir dir;
    const auto    path = dir.filePath(QStringLiteral("omemo.state"));
    const auto    key  = SecureEnvelope::generateMasterKey();
    {
        XmppOmemoStorage storage(path, key, QStringLiteral("me@example.org"));
        QVERIFY(storage.isValid());
        QXmppOmemoStorage::OwnDevice own;
        own.id                 = 42;
        own.label              = QStringLiteral("Secret device label");
        own.privateIdentityKey = QByteArrayLiteral("private identity material");
        own.publicIdentityKey  = QByteArrayLiteral("public identity material");
        storage.setOwnDevice(own);
        QXmppOmemoStorage::Device peer;
        peer.label   = QStringLiteral("Other device");
        peer.keyId   = QByteArrayLiteral("peer-key-id");
        peer.session = QByteArrayLiteral("session-secret");
        storage.addDevice(QStringLiteral("me@example.org"), 7, peer);
        QVERIFY(storage.isValid());
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto encrypted = file.readAll();
    QVERIFY(!encrypted.contains("private identity material"));
    QVERIFY(!encrypted.contains("session-secret"));

    XmppOmemoStorage restored(path, key, QStringLiteral("me@example.org"));
    QVERIFY2(restored.isValid(), qPrintable(restored.errorString()));
    auto task = restored.allData();
    QVERIFY(task.isFinished());
    auto data = task.takeResult();
    QVERIFY(data.ownDevice.has_value());
    QCOMPARE(data.ownDevice->id, uint32_t(42));
    QCOMPARE(data.devices[QStringLiteral("me@example.org")][7].session, QByteArrayLiteral("session-secret"));
}

void XmppOmemoStorageTest::clearsPeerSessionsWithoutResettingOwnDevice()
{
    QTemporaryDir dir;
    const auto    path = dir.filePath(QStringLiteral("omemo.state"));
    const auto    key  = SecureEnvelope::generateMasterKey();

    XmppOmemoStorage             storage(path, key, QStringLiteral("me@example.org"));
    QXmppOmemoStorage::OwnDevice own;
    own.id                 = 42;
    own.privateIdentityKey = QByteArrayLiteral("private identity material");
    own.publicIdentityKey  = QByteArrayLiteral("public identity material");
    storage.setOwnDevice(own);

    QXmppOmemoStorage::Device peer;
    peer.session = QByteArrayLiteral("stale session");
    storage.addDevice(QStringLiteral("me@example.org"), 7, peer);
    storage.addDevice(QStringLiteral("me@example.org/AnyKeep-other"), 8, peer);

    auto cleared = storage.removeAllDevices();
    QVERIFY(cleared.isFinished());

    auto dataTask = storage.allData();
    QVERIFY(dataTask.isFinished());
    const auto data = dataTask.takeResult();
    QVERIFY(data.devices.isEmpty());
    QVERIFY(data.ownDevice.has_value());
    QCOMPARE(data.ownDevice->id, uint32_t(42));
    QCOMPARE(data.ownDevice->privateIdentityKey, QByteArrayLiteral("private identity material"));
}

void XmppOmemoStorageTest::resetsSessionsWithoutRemovingFingerprints()
{
    QTemporaryDir             dir;
    XmppOmemoStorage          storage(dir.filePath(QStringLiteral("omemo.state")), SecureEnvelope::generateMasterKey(),
                                      QStringLiteral("me@example.org"));
    QXmppOmemoStorage::Device peer;
    peer.label                           = QStringLiteral("Other AnyKeep");
    peer.keyId                           = QByteArrayLiteral("public identity key");
    peer.session                         = QByteArrayLiteral("stale ratchet session");
    peer.unrespondedSentStanzasCount     = 3;
    peer.unrespondedReceivedStanzasCount = 4;
    storage.addDevice(QStringLiteral("me@example.org"), 7, peer);

    auto reset = storage.resetAllSessions();
    QVERIFY(reset.isFinished());

    auto dataTask = storage.allData();
    QVERIFY(dataTask.isFinished());
    const auto device = dataTask.takeResult().devices[QStringLiteral("me@example.org")][7];
    QCOMPARE(device.label, QStringLiteral("Other AnyKeep"));
    QCOMPARE(device.keyId, QByteArrayLiteral("public identity key"));
    QVERIFY(device.session.isEmpty());
    QCOMPARE(device.unrespondedSentStanzasCount, 0);
    QCOMPARE(device.unrespondedReceivedStanzasCount, 0);
}

void XmppOmemoStorageTest::preservesIdentityOnPartialOwnDeviceUpdate()
{
    QTemporaryDir    dir;
    XmppOmemoStorage storage(dir.filePath(QStringLiteral("omemo.state")), SecureEnvelope::generateMasterKey(),
                             QStringLiteral("me@example.org"));
    QXmppOmemoStorage::OwnDevice own;
    own.id                 = 42;
    own.label              = QStringLiteral("AnyKeep-device");
    own.privateIdentityKey = QByteArrayLiteral("private identity");
    own.publicIdentityKey  = QByteArrayLiteral("public identity");
    storage.setOwnDevice(own);

    QXmppOmemoStorage::OwnDevice partial;
    partial.id                   = 42;
    partial.latestSignedPreKeyId = 8;
    storage.setOwnDevice(partial);

    auto dataTask = storage.allData();
    QVERIFY(dataTask.isFinished());
    const auto updated = dataTask.takeResult().ownDevice;
    QVERIFY(updated.has_value());
    QCOMPARE(updated->label, QStringLiteral("AnyKeep-device"));
    QCOMPARE(updated->privateIdentityKey, QByteArrayLiteral("private identity"));
    QCOMPARE(updated->publicIdentityKey, QByteArrayLiteral("public identity"));
    QCOMPARE(updated->latestSignedPreKeyId, uint32_t(8));
}

void XmppOmemoStorageTest::rejectsWrongLocalKey()
{
    QTemporaryDir dir;
    const auto    path = dir.filePath(QStringLiteral("omemo.state"));
    {
        XmppOmemoStorage storage(path, SecureEnvelope::generateMasterKey(), QStringLiteral("me@example.org"));
        QXmppOmemoStorage::OwnDevice own;
        own.id = 1;
        storage.setOwnDevice(own);
    }
    XmppOmemoStorage wrong(path, SecureEnvelope::generateMasterKey(), QStringLiteral("me@example.org"));
    QVERIFY(!wrong.isValid());
}

QTEST_GUILESS_MAIN(XmppOmemoStorageTest)
#include "xmppomemostorage_test.moc"
