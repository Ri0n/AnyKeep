#include "localmediastore.h"
#include "nextcloudstorage.h"
#include "secureenvelope.h"

#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace AnyKeep;

class NextcloudMediaGuardTest final : public QObject {
    Q_OBJECT
private slots:
    void mediaManifestCannotBeDroppedOnSave();
    void missingLocalContentCannotBePublished();
    void modifiedExternalSourceCannotBePublished();
    void insecureRemoteUrlIsRejected();
};

void NextcloudMediaGuardTest::mediaManifestCannotBeDroppedOnSave()
{
    NextcloudStorage storage;
    auto note = storage.createNote();
    note.setText(QStringLiteral("Plain text"), Note::Markdown);
    MediaReference reference;
    reference.id = QUuid::createUuid();
    reference.portableName = QStringLiteral("picture.png");
    note.setMedia({ reference });
    auto *job = storage.saveNoteAsync(note);
    QVERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    QCOMPARE(job->error().code, StorageError::Other);
    QVERIFY(!job->error().retryable);
    job->deleteLater();
}

void NextcloudMediaGuardTest::missingLocalContentCannotBePublished()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.path(), SecureEnvelope::generateMasterKey());
    // Stream preflight now runs asynchronously, but must still fail before
    // sending any HTTP request when the referenced local source is missing.
    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), QStringLiteral("http://127.0.0.1:1"));
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("password"));
    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("some-etag"));

    MediaReference reference;
    reference.id = QUuid::createUuid();
    reference.portableName = QStringLiteral("lost.png");
    reference.blobId = QByteArray(32, 'b');
    reference.checksum = QByteArray(32, 'c');
    reference.size = 42;
    note.setMedia({ reference });
    note.setText(QStringLiteral("![image](") + reference.uri() + QLatin1Char(')'), Note::Markdown);
    auto *job = storage.saveNoteAsync(note);
    QTRY_VERIFY_WITH_TIMEOUT(job->isFinished(), 10000);
    QCOMPARE(job->state(), StorageJob::Failed);
    QCOMPARE(job->error().code, StorageError::Other);
    QVERIFY(job->error().message.contains(QStringLiteral("local media"), Qt::CaseInsensitive));
    job->deleteLater();
    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
}

void NextcloudMediaGuardTest::modifiedExternalSourceCannotBePublished()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore mediaStore(directory.filePath(QStringLiteral("store")),
                               SecureEnvelope::generateMasterKey());
    const QString externalFile = directory.filePath(QStringLiteral("linked.png"));
    {
        QFile source(externalFile);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(QByteArrayLiteral("first-revision")), qint64(14));
    }
    const auto linked = mediaStore.referenceFile(externalFile);
    QVERIFY2(linked, qPrintable(linked.error));
    {
        // A different size guarantees a changed source revision even on
        // filesystems whose last-modified time has coarse precision.
        QFile source(externalFile);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write(QByteArrayLiteral("second-revision-longer")), qint64(22));
    }

    QSettings settings;
    settings.setValue(QStringLiteral("storage.nextcloud.url"), QStringLiteral("http://127.0.0.1:1"));
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("password"));

    NextcloudStorage storage(nullptr, nullptr, &mediaStore);
    auto note = storage.createNote();
    note.setId(QStringLiteral("42"));
    note.setBackendValue(QStringLiteral("etag"), QStringLiteral("old-etag"));
    note.setText(QStringLiteral("![changed](") + linked.value.uri() + QLatin1Char(')'), Note::Markdown);
    note.setMedia({ linked.value });
    auto *saved = storage.saveNoteAsync(note);
    QTRY_VERIFY_WITH_TIMEOUT(saved->isFinished(), 10000);
    QCOMPARE(saved->state(), StorageJob::Failed);
    QCOMPARE(saved->error().code, StorageError::Other);
    QVERIFY(!saved->error().retryable);
    saved->deleteLater();

    settings.remove(QStringLiteral("storage.nextcloud.url"));
    settings.remove(QStringLiteral("storage.nextcloud.username"));
    settings.remove(QStringLiteral("storage.nextcloud.appPassword"));
}

void NextcloudMediaGuardTest::insecureRemoteUrlIsRejected()
{
    QSettings settings;
    const auto previousUrl = settings.value(QStringLiteral("storage.nextcloud.url"));
    const auto previousUser = settings.value(QStringLiteral("storage.nextcloud.username"));
    const auto previousPassword = settings.value(QStringLiteral("storage.nextcloud.appPassword"));
    settings.setValue(QStringLiteral("storage.nextcloud.url"), QStringLiteral("http://example.org"));
    settings.setValue(QStringLiteral("storage.nextcloud.username"), QStringLiteral("tester"));
    settings.setValue(QStringLiteral("storage.nextcloud.appPassword"), QStringLiteral("secret"));

    NextcloudStorage storage;
    auto *job = storage.initAsync();
    QVERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    QCOMPARE(job->error().code, StorageError::NotConfigured);
    QVERIFY(job->error().message.contains(QStringLiteral("HTTPS")));
    job->deleteLater();

    const auto restore = [&settings](const QString &key, const QVariant &value) {
        if (value.isValid())
            settings.setValue(key, value);
        else
            settings.remove(key);
    };
    restore(QStringLiteral("storage.nextcloud.url"), previousUrl);
    restore(QStringLiteral("storage.nextcloud.username"), previousUser);
    restore(QStringLiteral("storage.nextcloud.appPassword"), previousPassword);
}

QTEST_GUILESS_MAIN(NextcloudMediaGuardTest)
#include "nextcloudmediaguard_test.moc"
