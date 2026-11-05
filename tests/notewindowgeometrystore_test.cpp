#include <QTemporaryDir>
#include <QtTest>

#include "notewindowgeometrystore.h"

using namespace AnyKeep;

class NoteWindowGeometryStoreTest : public QObject {
    Q_OBJECT

    QTemporaryDir directory_;
    QString       path() const { return directory_.filePath(QStringLiteral("note-window-geometry.json")); }

private slots:
    void cleanup() { QFile::remove(path()); }

    void windowIdsComeFromNoteOrDraft()
    {
        const QUuid draft = QUuid::createUuid();
        QCOMPARE(NoteWindowGeometryStore::windowId(QStringLiteral("xmpp"), QStringLiteral("n1"), draft),
                 QStringLiteral("xmpp/n1"));
        QCOMPARE(NoteWindowGeometryStore::windowId(QStringLiteral("xmpp"), QString(), draft),
                 QStringLiteral("draft/") + draft.toString(QUuid::WithoutBraces));
        QVERIFY(NoteWindowGeometryStore::windowId(QString(), QString(), QUuid()).isEmpty());
    }

    void stateSurvivesRestart()
    {
        const QUuid draft = QUuid::createUuid();
        {
            NoteWindowGeometryStore store(path());
            store.setGeometry(QStringLiteral("ptf/a"), QRect(10, 20, 400, 300), draft);
            store.setKeepAbove(QStringLiteral("ptf/a"), true);
        }
        NoteWindowGeometryStore store(path());
        const auto              entry = store.entry(QStringLiteral("ptf/a"));
        QVERIFY(entry);
        QCOMPARE(entry->geometry, QRect(10, 20, 400, 300));
        QVERIFY(entry->keepAbove);
        QCOMPARE(entry->draftId, draft);
    }

    void writesAreDeferredAndCoalesced()
    {
        NoteWindowGeometryStore store(path());
        for (int x = 0; x < 50; ++x)
            store.setGeometry(QStringLiteral("ptf/a"), QRect(x, 0, 400, 300));
        // A window being dragged does not rewrite the file for every step.
        QVERIFY(!QFile::exists(path()));
        QTRY_VERIFY(QFile::exists(path()));
        NoteWindowGeometryStore reloaded(path());
        QCOMPARE(reloaded.entry(QStringLiteral("ptf/a"))->geometry, QRect(49, 0, 400, 300));
    }

    void publishedDraftCarriesItsWindowState()
    {
        NoteWindowGeometryStore store(path());
        const QUuid             draft   = QUuid::createUuid();
        const QString           draftId = NoteWindowGeometryStore::windowId({}, {}, draft);
        const QString           noteId  = QStringLiteral("xmpp/n1");
        // A compositor helper reports the geometry without knowing the draft.
        store.markShown(draftId, draft);
        store.setGeometry(draftId, QRect(5, 6, 320, 240));
        store.setKeepAbove(draftId, true);

        // The window has closed; the draft is published afterwards.
        store.renameDraft(draft, noteId);
        QVERIFY(!store.toJson().contains(draftId));
        QCOMPARE(store.entry(noteId)->geometry, QRect(5, 6, 320, 240));
        QVERIFY(store.entry(noteId)->keepAbove);

        // The helper's final report under the draft id arrives late.
        store.setGeometry(draftId, QRect(7, 8, 330, 250));
        QCOMPARE(store.entry(noteId)->geometry, QRect(7, 8, 330, 250));
        QVERIFY(!store.toJson().contains(draftId));
    }

    void renameFollowsChainsAndReplacesTheTarget()
    {
        NoteWindowGeometryStore store(path());
        store.setGeometry(QStringLiteral("old/a"), QRect(1, 1, 400, 300));
        store.setGeometry(QStringLiteral("new/a"), QRect(2, 2, 400, 300));
        store.rename(QStringLiteral("old/a"), QStringLiteral("new/a"));
        QCOMPARE(store.entry(QStringLiteral("new/a"))->geometry, QRect(1, 1, 400, 300));
        store.rename(QStringLiteral("new/a"), QStringLiteral("other/a"));
        // Writes to either earlier id reach the latest one.
        store.setGeometry(QStringLiteral("old/a"), QRect(3, 3, 400, 300));
        QCOMPARE(store.entry(QStringLiteral("other/a"))->geometry, QRect(3, 3, 400, 300));
        QCOMPARE(store.toJson().size(), 1);
    }

    void removedWindowIsForgotten()
    {
        NoteWindowGeometryStore store(path());
        store.setGeometry(QStringLiteral("ptf/a"), QRect(1, 1, 400, 300));
        store.remove(QStringLiteral("ptf/a"));
        QVERIFY(!store.entry(QStringLiteral("ptf/a")));
        store.flush();
        NoteWindowGeometryStore reloaded(path());
        QVERIFY(!reloaded.entry(QStringLiteral("ptf/a")));
    }

    void onlyTheMostRecentlyUsedWindowsAreKept()
    {
        const int capacity = NoteWindowGeometryStore::MaxWindows;
        {
            NoteWindowGeometryStore store(path());
            for (int index = 0; index < capacity + 10; ++index)
                store.setGeometry(QStringLiteral("ptf/%1").arg(index), QRect(index, 0, 400, 300));
            QCOMPARE(store.toJson().size(), capacity);
            QVERIFY(!store.entry(QStringLiteral("ptf/9")));
            // Showing the oldest remaining window again keeps it; the next
            // oldest goes instead.
            store.markShown(QStringLiteral("ptf/10"), QUuid::createUuid());
            store.setGeometry(QStringLiteral("ptf/new"), QRect(1, 1, 400, 300));
            QVERIFY(store.entry(QStringLiteral("ptf/10")));
            QVERIFY(!store.entry(QStringLiteral("ptf/11")));
        }
        // The order of use survives a restart.
        NoteWindowGeometryStore store(path());
        QCOMPARE(store.toJson().size(), capacity);
        store.setGeometry(QStringLiteral("ptf/after-restart"), QRect(2, 2, 400, 300));
        QVERIFY(!store.entry(QStringLiteral("ptf/12")));
        QVERIFY(store.entry(QStringLiteral("ptf/10")));
        QVERIFY(store.entry(QStringLiteral("ptf/new")));
        QVERIFY(store.entry(QStringLiteral("ptf/after-restart")));
    }

    void stateWithoutGeometryIsNotPlaced()
    {
        NoteWindowGeometryStore store(path());
        store.markShown(QStringLiteral("ptf/a"), QUuid::createUuid());
        const QJsonObject json = store.toJson();
        QVERIFY(json.contains(QStringLiteral("ptf/a")));
        QVERIFY(!json.value(QStringLiteral("ptf/a")).toObject().contains(QStringLiteral("width")));
        QVERIFY(!store.entry(QStringLiteral("ptf/a"))->geometry.isValid());
    }
};

QTEST_GUILESS_MAIN(NoteWindowGeometryStoreTest)
#include "notewindowgeometrystore_test.moc"
