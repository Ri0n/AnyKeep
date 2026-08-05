#include <QtTest>

#include "localdatakeystore.h"
#include "localmediastore.h"
#include "ptfstorage.h"
#include "secureenvelope.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
constexpr auto TimeZoneUTC = QTimeZone::Initialization::UTC;
#else
constexpr auto TimeZoneUTC = Qt::UTC;
#endif

using namespace AnyKeep;

class PTFStorageTest : public QObject {
    Q_OBJECT

    static FolderCatalogSnapshot catalogWithFolder(const QUuid &folderId)
    {
        FolderRecord folder;
        folder.id         = folderId;
        folder.name       = QStringLiteral("Project");
        folder.revision   = 1;
        folder.modifiedAt = QDateTime::currentDateTimeUtc();

        FolderCatalogSnapshot snapshot;
        snapshot.folders.append(folder);
        return snapshot;
    }

private slots:
    void styledAndMarkdownImagesSurviveFreshStorageInstance()
    {
        QTemporaryDir notesDirectory;
        QTemporaryDir mediaDirectory;
        QVERIFY(notesDirectory.isValid());
        QVERIFY(mediaDirectory.isValid());

        const auto masterKey = SecureEnvelope::generateMasterKey();
        QCOMPARE(masterKey.size(), LocalDataKeyStore::MasterKeySize);
        LocalMediaStore  mediaWriter(mediaDirectory.path(), masterKey);
        const QByteArray imageBytes("not-decoded-by-the-storage-test");
        const auto       imported = mediaWriter.importData(imageBytes, QStringLiteral("diagram with spaces.png"),
                                                           QStringLiteral("image/png"));
        QVERIFY2(imported, qPrintable(imported.error));

        QString body = QStringLiteral("![Default](%1)\n\n"
                                      "<p align=\"right\"><img src=\"%1\" alt=\"Styled\" width=\"320\" /></p>")
                           .arg(imported.value.uri());
        {
            PTFStorage writer(mediaWriter);
            QVERIFY(writer.setStoragePath(notesDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Images"));
            note.setText(body, Note::Markdown);
            note.setMedia({ imported.value });
            QVERIFY(writer.saveNote(note));
        }

        LocalMediaStore mediaReader(mediaDirectory.path(), masterKey);
        PTFStorage      reader(mediaReader);
        QVERIFY(reader.setStoragePath(notesDirectory.path()));
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.media().size(), 1);
        QCOMPARE(loaded.media().constFirst().id, imported.value.id);
        const auto opened = mediaReader.data(loaded.media().constFirst().blobId);
        QVERIFY2(opened, qPrintable(opened.error));
        QCOMPARE(opened.value, imageBytes);
    }

    void audioAttachmentSurvivesFreshStorageInstance()
    {
        QTemporaryDir notesDirectory;
        QTemporaryDir mediaDirectory;
        QVERIFY(notesDirectory.isValid());
        QVERIFY(mediaDirectory.isValid());

        const auto       masterKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore  mediaWriter(mediaDirectory.path(), masterKey);
        const QByteArray audioBytes("encoded-audio-payload");
        const auto       imported
            = mediaWriter.importData(audioBytes, QStringLiteral("recording.m4a"), QStringLiteral("audio/mp4"));
        QVERIFY2(imported, qPrintable(imported.error));

        const QString body = QStringLiteral("<audio controls src=\"%1\" title=\"Voice memo\" "
                                            "data-anykeep-duration-ms=\"42000\"></audio>\n"
                                            "<div data-anykeep-audio-transcript=\"1\">Recorded transcript</div>")
                                 .arg(imported.value.uri());
        {
            PTFStorage writer(mediaWriter);
            QVERIFY(writer.setStoragePath(notesDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Audio"));
            note.setText(body, Note::Markdown);
            note.setMedia({ imported.value });
            QVERIFY(writer.saveNote(note));
        }

        LocalMediaStore mediaReader(mediaDirectory.path(), masterKey);
        PTFStorage      reader(mediaReader);
        QVERIFY(reader.setStoragePath(notesDirectory.path()));
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.media().size(), 1);
        QCOMPARE(loaded.media().constFirst().id, imported.value.id);
        QCOMPARE(loaded.media().constFirst().mediaType, QStringLiteral("audio/mp4"));
        const auto opened = mediaReader.data(loaded.media().constFirst().blobId);
        QVERIFY2(opened, qPrintable(opened.error));
        QCOMPARE(opened.value, audioBytes);
    }

    void genericAttachmentSurvivesFreshStorageInstance()
    {
        QTemporaryDir notesDirectory;
        QTemporaryDir mediaDirectory;
        QVERIFY(notesDirectory.isValid());
        QVERIFY(mediaDirectory.isValid());

        const auto       masterKey = SecureEnvelope::generateMasterKey();
        LocalMediaStore  mediaWriter(mediaDirectory.path(), masterKey);
        const QByteArray fileBytes("portable-document-payload");
        const auto       imported
            = mediaWriter.importData(fileBytes, QStringLiteral("specification.pdf"), QStringLiteral("application/pdf"));
        QVERIFY2(imported, qPrintable(imported.error));

        const QString body = QStringLiteral("<a href=\"%1\" data-anykeep-attachment=\"1\" "
                                            "data-anykeep-media-type=\"application/pdf\" data-anykeep-size=\"%2\">"
                                            "specification.pdf</a>")
                                 .arg(imported.value.uri(), QString::number(fileBytes.size()));
        {
            PTFStorage writer(mediaWriter);
            QVERIFY(writer.setStoragePath(notesDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Attachment"));
            note.setText(body, Note::Markdown);
            note.setMedia({ imported.value });
            QVERIFY(writer.saveNote(note));
        }

        LocalMediaStore mediaReader(mediaDirectory.path(), masterKey);
        PTFStorage      reader(mediaReader);
        QVERIFY(reader.setStoragePath(notesDirectory.path()));
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        Note loaded = notes.constFirst();
        QVERIFY(loaded.load());
        QCOMPARE(loaded.text(), body);
        QCOMPARE(loaded.media().size(), 1);
        QCOMPARE(loaded.media().constFirst().id, imported.value.id);
        QCOMPARE(loaded.media().constFirst().mediaType, QStringLiteral("application/pdf"));
        const auto opened = mediaReader.data(loaded.media().constFirst().blobId);
        QVERIFY2(opened, qPrintable(opened.error));
        QCOMPARE(opened.value, fileBytes);
    }

    void noteSurvivesFreshStorageInstance()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSettings  settings;
        const auto previousPath    = settings.value(QStringLiteral("storage.ptf.path"));
        const bool hadPreviousPath = settings.contains(QStringLiteral("storage.ptf.path"));

        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(directory.path()));

            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Persistent note"));
            note.setText(QStringLiteral("Persistent body"), Note::Markdown);
            QVERIFY(writer.saveNote(note));
            QCOMPARE(writer.noteList().size(), 1);
        }

        {
            PTFStorage reader;
            QVERIFY(reader.setStoragePath(directory.path()));
            const auto notes = reader.noteList();
            QCOMPARE(notes.size(), 1);
            QCOMPARE(notes.first().title(), QStringLiteral("Persistent note"));

            Note loaded = notes.first();
            QVERIFY(loaded.load());
            QCOMPARE(loaded.text(), QStringLiteral("Persistent body"));
            QCOMPARE(loaded.format(), Note::Markdown);
        }

        if (hadPreviousPath)
            settings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
        else
            settings.remove(QStringLiteral("storage.ptf.path"));
    }

    void legacyPtfMetadataIsMigrated()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSettings  settings;
        const auto previousPath    = settings.value(QStringLiteral("storage.ptf.path"));
        const bool hadPreviousPath = settings.contains(QStringLiteral("storage.ptf.path"));
        struct SettingsGuard {
            QVariant previousPath;
            bool     hadPreviousPath;
            ~SettingsGuard()
            {
                QSettings settings;
                if (hadPreviousPath)
                    settings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
                else
                    settings.remove(QStringLiteral("storage.ptf.path"));
            }
        } guard { previousPath, hadPreviousPath };

        QFile note(directory.filePath(QStringLiteral("legacy.md")));
        QVERIFY(note.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(note.write("Legacy\n<a href=\"qtnote-media:/00000000-0000-0000-0000-000000000001/file.pdf\" "
                           "data-qtnote-attachment=\"1\">file.pdf</a>")
                > 0);
        note.close();
        QFile legacyIndex(directory.filePath(QStringLiteral(".qtnote-folders.json")));
        QVERIFY(legacyIndex.open(QIODevice::WriteOnly));
        QVERIFY(legacyIndex.write(R"JSON({"version":1,"folders":[],"assignments":[]})JSON") > 0);
        legacyIndex.close();

        settings.setValue(QStringLiteral("storage.ptf.path"), directory.path());
        PTFStorage storage;
        QVERIFY(storage.init());
        QVERIFY(storage.folderCatalogAvailable());
        QVERIFY(storage.folderCatalogErrorString().isEmpty());

        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral(".qtnote-folders.json"))));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral(".anykeep-folders.json"))));
        QVERIFY(note.open(QIODevice::ReadOnly | QIODevice::Text));
        const auto contents = QString::fromUtf8(note.readAll());
        QVERIFY(contents.contains(QStringLiteral("anykeep-media:/")));
        QVERIFY(contents.contains(QStringLiteral("data-anykeep-attachment")));
    }

    void noteListDoesNotRetainPreInitDirectory()
    {
        QTemporaryDir storageDirectory;
        QTemporaryDir unrelatedDirectory;
        QVERIFY(storageDirectory.isValid());
        QVERIFY(unrelatedDirectory.isValid());

        QSettings settings;
        struct EnvironmentGuard {
            QVariant previousPath;
            bool     hadPreviousPath;
            QString  previousWorkingDirectory;

            ~EnvironmentGuard()
            {
                QSettings restoreSettings;
                if (hadPreviousPath)
                    restoreSettings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
                else
                    restoreSettings.remove(QStringLiteral("storage.ptf.path"));
                QDir::setCurrent(previousWorkingDirectory);
            }
        } guard { settings.value(QStringLiteral("storage.ptf.path")),
                  settings.contains(QStringLiteral("storage.ptf.path")), QDir::currentPath() };

        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(storageDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Initialized note"));
            note.setText(QStringLiteral("Persistent body"), Note::Markdown);
            QVERIFY(writer.saveNote(note));
        }

        QVERIFY(QDir::setCurrent(unrelatedDirectory.path()));
        PTFStorage reader;

        // A direct pre-init read uses QDir's default path, but FileStorage no
        // longer retains that result after init() selects the real directory.
        QCOMPARE(reader.noteList().size(), 0);

        QVERIFY(reader.init());
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().title(), QStringLiteral("Initialized note"));
    }

    void noteListReflectsExternalFileChanges()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QCOMPARE(storage.noteList().size(), 0);

        QFile external(directory.filePath(QStringLiteral("external.md")));
        QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(external.write("External title\nExternal body"), qint64(28));
        external.close();

        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().id(), QStringLiteral("external"));
        QCOMPARE(notes.first().title(), QStringLiteral("External title"));
    }

    void requestedModificationTimeSurvivesSave()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Ordered note"));
        note.setText(QStringLiteral("Body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        note = storage.noteList().first();
        QVERIFY(note.load());
        const auto requested = QDateTime::fromMSecsSinceEpoch(1'720'000'000'123, TimeZoneUTC);
        note.setLastChangeUTC(requested);
        note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), requested);
        QVERIFY(storage.saveNote(note));

        const auto saved = storage.noteList().first();
        QCOMPARE(saved.lastChangeUTC().toMSecsSinceEpoch(), requested.toMSecsSinceEpoch());
        QVERIFY(!saved.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).isValid());
    }

    void reorderChangesOnlyFileTimesAndPreservesBlockOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QVERIFY(storage.supportsNoteReordering());

        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Second"), baseTime.addSecs(-1)));
        QVERIFY(saveAt(QStringLiteral("Third"), baseTime.addSecs(-2)));

        const auto before = storage.noteList();
        QCOMPARE(before.size(), 3);
        QCOMPARE(before.at(0).title(), QStringLiteral("First"));
        QCOMPARE(before.at(1).title(), QStringLiteral("Second"));
        QCOMPARE(before.at(2).title(), QStringLiteral("Third"));

        QHash<QString, QByteArray> contents;
        for (const auto &note : before) {
            QFile file(note.backendValue(QStringLiteral("fileName")).toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            contents.insert(note.id(), file.readAll());
        }

        QSignalSpy invalidated(&storage, &NoteStorage::invalidated);
        auto      *job = storage.reorderNotesAsync({ before.at(0).id(), before.at(1).id() }, before.at(2).id());
        QVERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(invalidated.size(), 1);
        delete job;

        const auto after = storage.noteList();
        QCOMPARE(after.size(), 3);
        QCOMPARE(after.at(0).title(), QStringLiteral("Third"));
        QCOMPARE(after.at(1).title(), QStringLiteral("First"));
        QCOMPARE(after.at(2).title(), QStringLiteral("Second"));
        for (const auto &note : after) {
            QFile file(note.backendValue(QStringLiteral("fileName")).toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), contents.value(note.id()));
        }
    }

    void reorderUsesTheGapBeforeMovingNeighborTimestamps()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Second"), baseTime.addSecs(-4)));
        QVERIFY(saveAt(QStringLiteral("Third"), baseTime.addSecs(-8)));

        const auto before       = storage.noteList();
        const auto firstTimeMs  = before.at(0).lastChangeUTC().toMSecsSinceEpoch();
        const auto secondTimeMs = before.at(1).lastChangeUTC().toMSecsSinceEpoch();
        QSignalSpy invalidated(&storage, &NoteStorage::invalidated);

        auto *job = storage.reorderNoteAsync(before.at(2).id(), before.at(0).id());
        QVERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(invalidated.size(), 1);
        delete job;

        const auto after = storage.noteList();
        QCOMPARE(after.at(0).title(), QStringLiteral("First"));
        QCOMPARE(after.at(1).title(), QStringLiteral("Third"));
        QCOMPARE(after.at(2).title(), QStringLiteral("Second"));
        QCOMPARE(after.at(0).lastChangeUTC().toMSecsSinceEpoch(), firstTimeMs);
        QCOMPARE(after.at(2).lastChangeUTC().toMSecsSinceEpoch(), secondTimeMs);
        QVERIFY(after.at(1).lastChangeUTC() < after.at(0).lastChangeUTC());
        QVERIFY(after.at(1).lastChangeUTC() > after.at(2).lastChangeUTC());
    }

    void asyncSaveReturnsFinalIdForSubsequentReorder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Last"), baseTime.addSecs(-1)));
        const auto before = storage.noteList();
        QCOMPARE(before.size(), 2);

        Note transferred = storage.createNote();
        QVERIFY(transferred.id().isEmpty());
        transferred.setTitle(QStringLiteral("Transferred"));
        transferred.setText(QStringLiteral("Transferred body"), Note::Markdown);
        auto *saveJob = storage.saveNoteAsync(transferred);
        QTRY_VERIFY(saveJob->isFinished());
        QCOMPARE(saveJob->state(), StorageJob::Succeeded);
        const QString finalId = saveJob->result().id();
        QVERIFY(!finalId.isEmpty());
        const auto savedNotes = storage.noteList();
        QVERIFY(std::ranges::any_of(savedNotes, [&finalId](const Note &note) { return note.id() == finalId; }));
        delete saveJob;

        auto *reorderJob = storage.reorderNoteAsync(finalId, before.constLast().id());
        QVERIFY(reorderJob->isFinished());
        QCOMPARE(reorderJob->state(), StorageJob::Succeeded);
        delete reorderJob;

        const auto after = storage.noteList();
        QCOMPARE(after.size(), 3);
        QCOMPARE(after.constLast().id(), finalId);
        QCOMPARE(after.constLast().title(), QStringLiteral("Transferred"));
    }

    void folderMetadataRoundTripsWithoutRewritingTheNote()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QVERIFY(storage.supportsNativeFolders());
        QVERIFY(storage.supportsNativeFolderCatalog());

        const QUuid folderId = QUuid::createUuid();
        auto       *catalog  = storage.replaceNativeFolderCatalogAsync(catalogWithFolder(folderId));
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Folderable note"));
        note.setText(QStringLiteral("Body that must not be rewritten"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        const auto before = storage.noteList();
        QCOMPARE(before.size(), 1);
        Note       summary  = before.constFirst();
        const auto fileName = summary.backendValue(QStringLiteral("fileName")).toString();
        QFile      file(fileName);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto contents = file.readAll();
        file.close();
        const auto modified = QFileInfo(fileName).lastModified();

        summary.setFolderId(folderId);
        QSignalSpy changed(&storage, &NoteStorage::noteModified);
        auto      *move = storage.changeNoteFolderAsync(summary);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        QCOMPARE(move->result().folderId(), folderId);
        QCOMPARE(changed.size(), 1);
        delete move;

        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), contents);
        file.close();
        QCOMPARE(QFileInfo(fileName).lastModified(), modified);

        const auto moved = storage.noteList();
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved.constFirst().folderId(), folderId);

        PTFStorage reader;
        QVERIFY(reader.setStoragePath(directory.path()));
        QVERIFY(reader.folderCatalogAvailable());
        const auto afterRestart = reader.noteList();
        QCOMPARE(afterRestart.size(), 1);
        QCOMPARE(afterRestart.constFirst().folderId(), folderId);
    }

    void folderAssignmentFollowsNoteRename()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const QUuid folderId = QUuid::createUuid();
        auto       *catalog  = storage.replaceNativeFolderCatalogAsync(catalogWithFolder(folderId));
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Before rename"));
        note.setText(QStringLiteral("Body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        note = storage.noteList().constFirst();
        note.setFolderId(folderId);
        auto *move = storage.changeNoteFolderAsync(note);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        delete move;

        const auto oldNoteId = note.id();
        QVERIFY(note.load());
        note.setTitle(QStringLiteral("After rename"));
        QVERIFY(storage.saveNote(note));

        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        const auto renamed = notes.constFirst();
        QVERIFY(renamed.id() != oldNoteId);
        QCOMPARE(renamed.folderId(), folderId);

        FolderCatalog catalogView;
        QVERIFY(!catalogView.replaceSnapshot(storage.nativeFolderCatalog()));
        const auto *oldAssignment = catalogView.assignment(PTFStorage::storageId, oldNoteId);
        QVERIFY(oldAssignment);
        QVERIFY(oldAssignment->tombstone);
        QCOMPARE(catalogView.folderForNote(PTFStorage::storageId, renamed.id()), folderId);

        PTFStorage reader;
        QVERIFY(reader.setStoragePath(directory.path()));
        const auto persisted = reader.noteList();
        QCOMPARE(persisted.size(), 1);
        QCOMPARE(persisted.constFirst().folderId(), folderId);
    }

    void corruptFolderIndexDoesNotBlockNotesAndCanRestoreBackup()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const QUuid folderId = QUuid::createUuid();
        auto       *catalog  = storage.replaceNativeFolderCatalogAsync(catalogWithFolder(folderId));
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Recoverable folder index"));
        note.setText(QStringLiteral("Body remains readable"), Note::Markdown);
        QVERIFY(storage.saveNote(note));
        note = storage.noteList().constFirst();
        note.setFolderId(folderId);
        auto *move = storage.changeNoteFolderAsync(note);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        delete move;

        // Save the healthy primary once more so it is also retained as a
        // backup before simulating a damaged primary file.
        catalog = storage.replaceNativeFolderCatalogAsync(storage.nativeFolderCatalog());
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        const auto indexPath = directory.filePath(QStringLiteral(".anykeep-folders.json"));
        QVERIFY(QFile::exists(indexPath));
        QVERIFY(QFile::exists(indexPath + QStringLiteral(".bak")));
        QFile damaged(indexPath);
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(damaged.write("not a folder index") > 0);
        damaged.close();

        PTFStorage reader;
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 ".*PTF folder index is unavailable.*Invalid PTF folder index payload.*")));
        QVERIFY(reader.setStoragePath(directory.path()));
        QVERIFY(reader.isAccessible());
        QVERIFY(!reader.folderCatalogAvailable());
        QVERIFY(!reader.folderCatalogErrorString().isEmpty());
        const auto withoutFolders = reader.noteList();
        QCOMPARE(withoutFolders.size(), 1);
        QVERIFY(withoutFolders.constFirst().folderId().isNull());
        Note readable = withoutFolders.constFirst();
        QVERIFY(readable.load());
        QCOMPARE(readable.text(), QStringLiteral("Body remains readable"));

        QString    preservedPath;
        const auto restore = reader.restoreFolderCatalogBackup(&preservedPath);
        QVERIFY(!restore);
        QVERIFY(!preservedPath.isEmpty());
        QVERIFY(QFile::exists(preservedPath));
        QVERIFY(reader.folderCatalogAvailable());
        const auto restored = reader.noteList();
        QCOMPARE(restored.size(), 1);
        QCOMPARE(restored.constFirst().folderId(), folderId);
    }

    void corruptFolderIndexDoesNotTurnACommittedBodySaveIntoFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QUuid  firstFolder  = QUuid::createUuid();
        const QUuid  secondFolder = QUuid::createUuid();
        auto         snapshot     = catalogWithFolder(firstFolder);
        FolderRecord second;
        second.id         = secondFolder;
        second.name       = QStringLiteral("Later target");
        second.revision   = 1;
        second.modifiedAt = QDateTime::currentDateTimeUtc();
        snapshot.folders.append(second);

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        auto *catalog = storage.replaceNativeFolderCatalogAsync(snapshot);
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Save despite index failure"));
        note.setText(QStringLiteral("Original body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));
        note = storage.noteList().constFirst();
        note.setFolderId(firstFolder);
        auto *move = storage.changeNoteFolderAsync(note);
        QVERIFY(move->isFinished());
        QCOMPARE(move->state(), StorageJob::Succeeded);
        delete move;

        // Retain the assignment in the backup, then damage only the primary.
        catalog = storage.replaceNativeFolderCatalogAsync(storage.nativeFolderCatalog());
        QVERIFY(catalog->isFinished());
        QCOMPARE(catalog->state(), StorageJob::Succeeded);
        delete catalog;
        QFile damaged(directory.filePath(QStringLiteral(".anykeep-folders.json")));
        QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(damaged.write("corrupt") > 0);
        damaged.close();

        QVERIFY(note.load());
        note.setFolderId(secondFolder);
        note.setText(QStringLiteral("Committed body"), Note::Markdown);
        QSignalSpy errors(&storage, &NoteStorage::storageErorr);
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral(
                                 ".*Failed to update PTF folder index.*Invalid PTF folder index payload.*")));
        QVERIFY(storage.saveNote(note));
        QCOMPARE(errors.size(), 1);
        QVERIFY(!storage.folderCatalogAvailable());

        Note saved = storage.note(note.id());
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.text(), QStringLiteral("Committed body"));
    }
};

QTEST_MAIN(PTFStorageTest)

#include "ptfstorage_test.moc"
