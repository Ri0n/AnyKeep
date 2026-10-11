#include "nextcloudworker.h"
#include "nextcloudmedialinks.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QUuid>
#include <QtTest>

using namespace AnyKeep;

namespace {

NextcloudConfig liveConfig()
{
    NextcloudConfig config;
    config.serverUrl   = QUrl::fromUserInput(qEnvironmentVariable("NEXTCLOUD_TEST_URL"));
    config.userName    = qEnvironmentVariable("NEXTCLOUD_TEST_USER");
    config.appPassword = qEnvironmentVariable("NEXTCLOUD_TEST_PASSWORD");
    config.timeoutMs   = 30000;
    return config;
}

// Ensures failed assertions do not leave notes behind on a reused test server.
struct CleanupNote {
    NextcloudWorker &worker;
    QString id;

    ~CleanupNote()
    {
        if (!id.isEmpty())
            worker.deleteNote(id);
    }
};

} // namespace

class NextcloudLiveTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void createReadUpdateConflictDelete();
    void invalidPasswordIsRejected();
    void nativeAttachmentRoundtrip();
    void mediaLinkCodec();
    void chunkedWebDavUploadRoundtrip();
    void webDavNativeAttachmentSurvivesCategoryMove();
};

void NextcloudLiveTest::initTestCase()
{
    const auto config = liveConfig();
    QVERIFY2(config.serverUrl.isValid() && !config.serverUrl.host().isEmpty(),
             "Set NEXTCLOUD_TEST_URL to the live Nextcloud base URL");
    QVERIFY2(!config.userName.isEmpty(), "Set NEXTCLOUD_TEST_USER");
    QVERIFY2(!config.appPassword.isEmpty(), "Set NEXTCLOUD_TEST_PASSWORD");
}

void NextcloudLiveTest::createReadUpdateConflictDelete()
{
    NextcloudWorker worker;
    worker.setConfig(liveConfig());

    const auto probe = worker.probe();
    QVERIFY2(probe.ok, qPrintable(probe.error));

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString title = QStringLiteral("AnyKeep integration ") + token;
    const QString originalContent = QStringLiteral("# ") + title + QStringLiteral("\nOriginal body ") + token;
    const QString changedContent = QStringLiteral("# ") + title + QStringLiteral("\nUpdated body ") + token;

    NextcloudRemoteNote draft;
    draft.title          = title;
    draft.content        = originalContent;
    draft.contentPresent = true;
    draft.category       = QStringLiteral("AnyKeep Integration/Original");
    draft.favorite       = false;

    const auto created = worker.createNote(draft);
    QVERIFY2(created.ok, qPrintable(created.error));
    QVERIFY2(!created.note.id.isEmpty(), "The server must assign a note ID");
    CleanupNote cleanup { worker, created.note.id };

    // The list endpoint is deliberately metadata-only; content must be lazy.
    const auto listed = worker.listNotes();
    QVERIFY2(listed.ok, qPrintable(listed.error));
    bool listedCreated = false;
    for (const auto &item : listed.notes) {
        if (item.id != created.note.id)
            continue;
        listedCreated = true;
        QVERIFY2(!item.contentPresent, "exclude=content must be respected");
        QCOMPARE(item.category, draft.category);
        QCOMPARE(item.favorite, false);
    }
    QVERIFY2(listedCreated, "Created note not present in the remote notes list");

    const auto fetched = worker.getNote(created.note.id);
    QVERIFY2(fetched.ok, qPrintable(fetched.error));
    QCOMPARE(fetched.note.id, created.note.id);
    QVERIFY(fetched.note.contentPresent);
    QCOMPARE(fetched.note.content, originalContent);
    QVERIFY2(!fetched.note.etag.isEmpty(), "Note ETag needed for optimistic concurrency");

    const QString newCategory = QStringLiteral("AnyKeep Integration/Moved");
    const auto moved = worker.updateNoteCategory(fetched.note.id, newCategory, fetched.note.etag);
    QVERIFY2(moved.ok, qPrintable(moved.error));
    QCOMPARE(moved.note.category, newCategory);

    const auto afterMove = worker.getNote(created.note.id);
    QVERIFY2(afterMove.ok, qPrintable(afterMove.error));
    QCOMPARE(afterMove.note.category, newCategory);
    QCOMPARE(afterMove.note.content, originalContent);

    NextcloudRemoteNote edit = afterMove.note;
    edit.content             = changedContent;
    edit.favorite            = true;
    const auto saved = worker.updateNote(edit);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    QCOMPARE(saved.note.favorite, true);

    const auto updated = worker.getNote(created.note.id);
    QVERIFY2(updated.ok, qPrintable(updated.error));
    QCOMPARE(updated.note.content, changedContent);
    QCOMPARE(updated.note.category, newCategory);
    QCOMPARE(updated.note.favorite, true);

    // Reuse an old ETag after the full update. The server must reject the
    // stale write, and the worker must surface the current server version.
    NextcloudRemoteNote stale = edit;
    stale.content = QStringLiteral("# ") + title + QStringLiteral("\nSTALE WRITE");
    const auto conflict = worker.updateNote(stale);
    QVERIFY(!conflict.ok);
    QCOMPARE(conflict.httpStatus, 412);
    QVERIFY(conflict.conflict);
    QVERIFY(conflict.remoteOnConflict.has_value());
    QCOMPARE(conflict.remoteOnConflict->content, changedContent);

    const auto stillSaved = worker.getNote(created.note.id);
    QVERIFY2(stillSaved.ok, qPrintable(stillSaved.error));
    QCOMPARE(stillSaved.note.content, changedContent);

    const auto removed = worker.deleteNote(created.note.id);
    QVERIFY2(removed.ok, qPrintable(removed.error));
    cleanup.id.clear();

    const auto missing = worker.getNote(created.note.id);
    QVERIFY(!missing.ok);
    QCOMPARE(missing.httpStatus, 404);

    const auto listAfterDelete = worker.listNotes();
    QVERIFY2(listAfterDelete.ok, qPrintable(listAfterDelete.error));
    for (const auto &item : listAfterDelete.notes)
        QVERIFY(item.id != created.note.id);
}

void NextcloudLiveTest::invalidPasswordIsRejected()
{
    NextcloudConfig config = liveConfig();
    config.appPassword += QStringLiteral("-incorrect");

    NextcloudWorker worker;
    worker.setConfig(config);

    const auto result = worker.probe();
    QVERIFY(!result.ok);
    QCOMPARE(result.httpStatus, 401);
}

void NextcloudLiveTest::nativeAttachmentRoundtrip()
{
    NextcloudWorker worker;
    worker.setConfig(liveConfig());

    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    NextcloudRemoteNote draft;
    draft.title = QStringLiteral("AnyKeep attachment ") + unique;
    draft.content = QStringLiteral("# Binary attachment test ") + unique;
    draft.contentPresent = true;
    draft.category = QStringLiteral("AnyKeep Integration/Files");

    const auto created = worker.createNote(draft);
    QVERIFY2(created.ok, qPrintable(created.error));
    CleanupNote cleanup { worker, created.note.id };

    const QByteArray bytes = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489");
    const auto badName = worker.uploadAttachment(created.note.id, QStringLiteral("../secret"), bytes);
    QVERIFY(!badName.ok);
    QCOMPARE(badName.httpStatus, 0);

    const auto uploaded = worker.uploadAttachment(created.note.id, QStringLiteral("test.png"), bytes);
    QVERIFY2(uploaded.ok, qPrintable(uploaded.error));
    QVERIFY2(!uploaded.path.isEmpty(), "The server should return an attachment path");

    const auto fetched = worker.downloadAttachment(created.note.id, uploaded.path);
    QVERIFY2(fetched.ok, qPrintable(fetched.error));
    QCOMPARE(fetched.data, bytes);

    const auto settings = worker.getSettings();
    QVERIFY2(settings.ok, qPrintable(settings.error));
    const QString davPath = settings.notesPath + QLatin1Char('/')
        + draft.category + QLatin1Char('/') + uploaded.path;
    const auto firstRange = worker.readWebDavRange(davPath, 0, 12);
    QVERIFY2(firstRange.ok, qPrintable(firstRange.error));
    QCOMPARE(firstRange.totalSize, qint64(bytes.size()));
    QCOMPARE(firstRange.data, bytes.left(12));
    const auto secondRange = worker.readWebDavRange(davPath, 12, 13);
    QVERIFY2(secondRange.ok, qPrintable(secondRange.error));
    QCOMPARE(secondRange.data, bytes.mid(12, 13));

    // Save a standard relative image link, not an AnyKeep-only URI. The
    // server should preserve it exactly for native Notes clients.
    NextcloudRemoteNote note = created.note;
    note.content = QStringLiteral("# Binary attachment test ") + unique + QStringLiteral("\n![image](")
        + uploaded.path + QStringLiteral(")");
    const auto linked = worker.updateNote(note);
    QVERIFY2(linked.ok, qPrintable(linked.error));
    const auto reopened = worker.getNote(created.note.id);
    QVERIFY2(reopened.ok, qPrintable(reopened.error));
    QCOMPARE(reopened.note.content, note.content);

    // Notes categories correspond to native directories. A metadata-only
    // category move must not leave its relative attachment link dangling.
    const QString destination = QStringLiteral("AnyKeep Integration/MovedFiles");
    const auto moved = worker.updateNoteCategory(created.note.id, destination, reopened.note.etag);
    QVERIFY2(moved.ok, qPrintable(moved.error));
    const auto movedNote = worker.getNote(created.note.id);
    QVERIFY2(movedNote.ok, qPrintable(movedNote.error));
    QCOMPARE(movedNote.note.category, destination);
    QCOMPARE(movedNote.note.content, note.content);
    // Notes 5.0.2's v1.4 upload endpoint places attachments alongside
    // the note instead of its move-aware .attachments.<id> directory.
    // The older API upload is still supported, but is not a safe transport
    // for AnyKeep media that must survive category changes.
    const auto nativeAfterMove = worker.downloadAttachment(created.note.id, uploaded.path);
    QVERIFY(!nativeAfterMove.ok);
    QCOMPARE(nativeAfterMove.httpStatus, 404);
}

void NextcloudLiveTest::mediaLinkCodec()
{
    const QString uri = QStringLiteral("anykeep-media:/01234567-89ab-cdef-0123-456789abcdef/picture.png");
    const QString body = QStringLiteral("![picture](") + uri + QStringLiteral(")\n<audio controls src=\"")
        + uri + QStringLiteral("\"></audio>\n```md\n![literal](") + uri + QStringLiteral(")\n```");
    QString error;
    const QString native = NextcloudMediaLinks::toNative(
        body, { { uri, QStringLiteral(".attachments.42/picture name.png") } }, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(native.contains(QStringLiteral("![picture](.attachments.42/picture%20name.png)")));
    QVERIFY(native.contains(QStringLiteral("src=\".attachments.42/picture%20name.png\"")));
    QVERIFY(native.contains(QStringLiteral("![literal](") + uri + QLatin1Char(')')));

    const auto restored = NextcloudMediaLinks::toCanonical(
        native, { { QStringLiteral(".attachments.42/picture name.png"), uri } });
    QCOMPARE(restored, body);

    const auto paths = NextcloudMediaLinks::referencedNativePaths(native);
    QCOMPARE(paths, QStringList({ QStringLiteral(".attachments.42/picture name.png") }));
    QVERIFY(!NextcloudMediaLinks::isSafeNativePath(QStringLiteral("../private")));
    QVERIFY(!NextcloudMediaLinks::isSafeNativePath(QStringLiteral("%2E%2E/private")));
    QVERIFY(!NextcloudMediaLinks::isSafeNativePath(QStringLiteral("https://external/file.png")));

    const QString failed = NextcloudMediaLinks::toNative(QStringLiteral("![x](") + uri + QLatin1Char(')'),
                                                         {}, &error);
    QVERIFY(failed.isEmpty());
    QVERIFY(!error.isEmpty());
}

void NextcloudLiveTest::chunkedWebDavUploadRoundtrip()
{
    NextcloudWorker worker;
    worker.setConfig(liveConfig());

    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    NextcloudRemoteNote draft;
    draft.title = QStringLiteral("AnyKeep chunked ") + unique;
    draft.content = QStringLiteral("# Chunked test ") + unique;
    draft.contentPresent = true;
    draft.category = QStringLiteral("AnyKeep Integration/LargeFiles");
    const auto note = worker.createNote(draft);
    QVERIFY2(note.ok, qPrintable(note.error));
    CleanupNote cleanup { worker, note.note.id };

    const auto settings = worker.getSettings();
    QVERIFY2(settings.ok, qPrintable(settings.error));
    const QString fileName = unique + QStringLiteral(".bin");
    const QString davPath = settings.notesPath + QLatin1Char('/')
        + draft.category + QLatin1Char('/') + fileName;

    QByteArray bytes(5 * 1024 * 1024 + 1024, char('x'));
    bytes.replace(0, 8, QByteArrayLiteral("ANYKEEP!"));
    bytes.replace(5 * 1024 * 1024, 8, QByteArrayLiteral("TAILTEST"));
    QBuffer source(&bytes);
    QVERIFY(source.open(QIODevice::ReadOnly));

    const auto uploaded = worker.uploadWebDavChunks(davPath, &source, bytes.size());
    QVERIFY2(uploaded.ok, qPrintable(uploaded.error));
    QCOMPARE(uploaded.path, davPath);
    QCOMPARE(source.pos(), qint64(bytes.size()));

    // The two reads straddle the chunk boundary. Only requested bytes
    // should be fetched by the plugin, regardless of the full file size.
    const auto head = worker.readWebDavRange(davPath, 0, 16);
    QVERIFY2(head.ok, qPrintable(head.error));
    QCOMPARE(head.totalSize, qint64(bytes.size()));
    QCOMPARE(head.data, bytes.left(16));

    const auto boundary = worker.readWebDavRange(davPath, 5 * 1024 * 1024 - 4, 24);
    QVERIFY2(boundary.ok, qPrintable(boundary.error));
    QCOMPARE(boundary.data, bytes.mid(5 * 1024 * 1024 - 4, 24));
}

void NextcloudLiveTest::webDavNativeAttachmentSurvivesCategoryMove()
{
    NextcloudWorker worker;
    worker.setConfig(liveConfig());
    const QString unique = QUuid::createUuid().toString(QUuid::WithoutBraces);
    NextcloudRemoteNote draft;
    draft.title = QStringLiteral("AnyKeep native DAV ") + unique;
    draft.content = QStringLiteral("# Native DAV attachment test ") + unique;
    draft.contentPresent = true;
    draft.category = QStringLiteral("AnyKeep Integration/WebDavOriginal");
    const auto created = worker.createNote(draft);
    QVERIFY2(created.ok, qPrintable(created.error));
    CleanupNote cleanup { worker, created.note.id };

    const auto settings = worker.getSettings();
    QVERIFY2(settings.ok, qPrintable(settings.error));
    const QString nativeDir = QStringLiteral(".attachments.") + created.note.id;
    const QString relativeName = nativeDir + QStringLiteral("/portable.bin");
    const QString directoryPath = settings.notesPath + QLatin1Char('/')
        + draft.category + QLatin1Char('/') + nativeDir;
    const auto mkcol = worker.ensureWebDavFolder(directoryPath);
    QVERIFY2(mkcol.ok, qPrintable(mkcol.error));
    const QByteArray bytes = QByteArrayLiteral("AN-YKEEP-NATIVE-WEBDAV-ATTACHMENT");
    QBuffer buffer;
    buffer.setData(bytes);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    const QString davFilePath = directoryPath + QStringLiteral("/portable.bin");
    const auto uploaded = worker.uploadWebDavChunks(davFilePath, &buffer, bytes.size());
    QVERIFY2(uploaded.ok, qPrintable(uploaded.error));
    const auto before = worker.readWebDavRange(davFilePath, 0, bytes.size());
    QVERIFY2(before.ok, qPrintable(before.error));
    QCOMPARE(before.data, bytes);

    auto linkedNote = created.note;
    linkedNote.content += QStringLiteral("\n[attachment](") + relativeName + QLatin1Char(')');
    const auto saved = worker.updateNote(linkedNote);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    const QString nextCategory = QStringLiteral("AnyKeep Integration/WebDavMoved");
    const auto moved = worker.updateNoteCategory(created.note.id, nextCategory, saved.note.etag);
    QVERIFY2(moved.ok, qPrintable(moved.error));

    const auto reloaded = worker.getNote(created.note.id);
    QVERIFY2(reloaded.ok, qPrintable(reloaded.error));
    QCOMPARE(reloaded.note.category, nextCategory);
    QCOMPARE(reloaded.note.content, linkedNote.content);
    const QString relocatedPath = settings.notesPath + QLatin1Char('/')
        + nextCategory + QLatin1Char('/') + relativeName;
    const auto after = worker.readWebDavRange(relocatedPath, 0, bytes.size());
    QVERIFY2(after.ok, qPrintable(after.error));
    QCOMPARE(after.data, bytes);
}

QTEST_GUILESS_MAIN(NextcloudLiveTest)
#include "nextcloud_live_test.moc"
