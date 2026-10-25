#include <QApplication>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QtTest>

#include "anykeepdbus.h"
#include "baseintegrationtray.h"
#include "notedata.h"
#include "notemanager.h"
#include "pluginhostinterface.h"

using namespace AnyKeep;

namespace {
class SearchStorage final : public NoteStorage {
public:
    bool                init() override { return true; }
    const QString       systemName() const override { return QStringLiteral("tray-search-test"); }
    const QString       name() const override { return QStringLiteral("Tray search test"); }
    QIcon               storageIcon() const override { return {}; }
    QIcon               noteIcon() const override { return {}; }
    bool                isAccessible() const override { return true; }
    QList<Note::Format> availableFormats() const override { return { Note::PlainText }; }
    QList<Note>         noteList(int = 0) override
    {
        auto summaries = notes;
        for (auto &summary : summaries)
            summary.unload();
        return summaries;
    }
    Note note(const QString &id) override
    {
        for (const auto &candidate : notes)
            if (candidate.id() == id)
                return candidate;
        return {};
    }
    Note createNote() override { return Note(new NoteData(this)); }
    bool saveNote(const Note &) override { return false; }
    void removeNote(const QString &) override {}

    NoteLoadJob *loadNoteAsync(const QString &id, QObject *owner = nullptr) override
    {
        ++loads;
        if (!holdLoads)
            return NoteStorage::loadNoteAsync(id, owner);
        auto *job = new NoteLoadJob(owner ? owner : this);
        job->start();
        heldLoads.append(job);
        return job;
    }

    void add(const QString &id, const QString &title, const QString &body, const QStringList &tags = {})
    {
        auto item = createNote();
        item.setId(id);
        item.setTitle(title);
        item.setText(body, Note::PlainText);
        item.setTags(tags);
        item.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        notes.append(item);
    }
    void announceChange() { emit invalidated(); }

    QList<Note>                  notes;
    QList<QPointer<NoteLoadJob>> heldLoads;
    bool                         holdLoads { false };
    int                          loads { 0 };
};

class TestHost final : public PluginHostInterface {
public:
    QString      utilsCuttedDots(const QString &text, int limit) override { return text.left(limit); }
    NoteManager *noteManager() override { return NoteManager::instance(); }
    QString      anykeepDataDir() override { return {}; }
    void         rehighlight() override {}
    bool         offerSpellCheckProvider(std::shared_ptr<SpellCheckProvider>) override { return false; }
};

QJsonObject response(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }

QStringList noteIds(const QString &json)
{
    QStringList result;
    for (const auto &value : response(json).value(QStringLiteral("notes")).toArray())
        result.append(value.toObject().value(QStringLiteral("id")).toString());
    result.sort();
    return result;
}

QWidget *trayPopup()
{
    for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QLatin1String("trayNotesPopup"))
            return widget;
    return nullptr;
}
} // namespace

class TrayNotesTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void init()
    {
        auto storage = std::make_unique<SearchStorage>();
        storage_     = storage.get();
        storage_->add(QStringLiteral("title"), QStringLiteral("Needle title"), QStringLiteral("ordinary"));
        storage_->add(QStringLiteral("body"), QStringLiteral("Body match"), QStringLiteral("secret NEEDLE here"));
        storage_->add(QStringLiteral("tag"), QStringLiteral("Tag match"), QStringLiteral("ordinary"),
                      { QStringLiteral("needle-tag") });
        storage_->add(QStringLiteral("other"), QStringLiteral("Other"), QStringLiteral("different contents"));
        NoteManager::instance()->registerStorage(std::move(storage));
        QTRY_COMPARE(NoteManager::instance()->noteList().size(), 4);
    }

    void cleanup() { NoteManager::instance()->unregisterStorage(storage_); }

    void dbusBodySearchKeepsMetadataFilteringAndPagination()
    {
        AnyKeepDBus bridge(nullptr);
        QSignalSpy  finished(&bridge, &AnyKeepDBus::bodySearchFinished);
        QCOMPARE(noteIds(bridge.notesJson(0, 50, QStringLiteral("needle"))),
                 QStringList({ QStringLiteral("tag"), QStringLiteral("title") }));
        QCOMPARE(storage_->loads, 0);
        QVERIFY(response(bridge.notesWithBodyJson(0, 50, QStringLiteral("needle")))
                    .value(QStringLiteral("searching"))
                    .toBool());
        bridge.notesWithBodyJson(0, 50, QStringLiteral("different"));
        QTRY_COMPARE(finished.count(), 2);
        const auto found = bridge.notesWithBodyJson(0, 50, QStringLiteral("needle"));
        QCOMPARE(noteIds(found),
                 QStringList({ QStringLiteral("body"), QStringLiteral("tag"), QStringLiteral("title") }));
        QVERIFY(!response(found).value(QStringLiteral("searching")).toBool());
        QCOMPARE(noteIds(bridge.notesWithBodyJson(0, 50, QStringLiteral("different"))),
                 QStringList({ QStringLiteral("other") }));

        const int   loads = storage_->loads;
        QStringList paged;
        for (int offset = 0; offset < 3; ++offset) {
            const auto page = bridge.notesWithBodyJson(offset, 1, QStringLiteral("needle"));
            paged.append(noteIds(page));
            QCOMPARE(response(page).value(QStringLiteral("hasMore")).toBool(), offset < 2);
        }
        paged.sort();
        QCOMPARE(paged, noteIds(found));
        QCOMPARE(storage_->loads, loads);
        QCOMPARE(noteIds(bridge.notesJson(0, 50, QStringLiteral("needle"))).size(), 2);
        const auto bus = QDBusConnection::sessionBus();
        if (bus.isConnected()) {
            auto call = QDBusMessage::createMethodCall(
                QStringLiteral("com.github.ri0n.AnyKeep"), QStringLiteral("/AnyKeep"),
                QStringLiteral("com.github.ri0n.AnyKeep"), QStringLiteral("notesWithBodyJson"));
            call.setArguments({ 0, 50, QStringLiteral("needle") });
            QDBusPendingCallWatcher watcher(bus.asyncCall(call));
            QTRY_VERIFY(watcher.isFinished());
            QDBusPendingReply<QString> reply = watcher;
            QVERIFY2(!reply.isError(), qPrintable(reply.error().message()));
            QCOMPARE(noteIds(reply.value()), noteIds(found));
        }
    }

    void dbusInvalidationAndDestructionCancelPendingBodyLoads()
    {
        storage_->holdLoads = true;
        auto bridge         = std::make_unique<AnyKeepDBus>(nullptr);
        bridge->notesWithBodyJson(0, 50, QStringLiteral("needle"));
        QTRY_COMPARE(storage_->heldLoads.size(), 1);
        auto *oldLoad = storage_->heldLoads.first().data();
        QVERIFY(oldLoad);
        QSignalSpy changed(bridge.get(), &AnyKeepDBus::notesChanged);
        storage_->announceChange();
        QCOMPARE(oldLoad->state(), StorageJob::Cancelled);
        QTRY_VERIFY(changed.count() >= 2);

        storage_->holdLoads = false;
        QSignalSpy finished(bridge.get(), &AnyKeepDBus::bodySearchFinished);
        bridge->notesWithBodyJson(0, 50, QStringLiteral("different"));
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().first().toString(), QStringLiteral("different"));
        QCOMPARE(noteIds(bridge->notesWithBodyJson(0, 50, QStringLiteral("different"))),
                 QStringList({ QStringLiteral("other") }));

        storage_->holdLoads = true;
        bridge->notesWithBodyJson(0, 50, QStringLiteral("uncached"));
        QTRY_COMPARE(storage_->heldLoads.size(), 2);
        auto *pending = storage_->heldLoads.last().data();
        QVERIFY(pending);
        QSignalSpy cancelled(pending, &StorageJob::finished);
        bridge.reset();
        QCOMPARE(cancelled.count(), 1);
    }

    void fallbackPopupFindsBodiesAndKeepsSearchFocus()
    {
        TestHost            host;
        BaseIntegrationTray tray(nullptr, &host);
        QVERIFY(QMetaObject::invokeMethod(&tray, "showNoteList",
                                          Q_ARG(QSystemTrayIcon::ActivationReason, QSystemTrayIcon::Trigger)));
        QPointer<QWidget> popup(trayPopup());
        QVERIFY(popup);
        const auto closePopup = qScopeGuard([&]() {
            if (popup)
                delete popup.data();
        });
        auto      *filter     = popup->findChild<QLineEdit *>(QStringLiteral("trayNotesSearchField"));
        auto      *check      = popup->findChild<QCheckBox *>(QStringLiteral("traySearchInText"));
        auto      *list       = popup->findChild<QListWidget *>(QStringLiteral("trayNotesList"));
        QVERIFY(filter);
        QVERIFY(check);
        QVERIFY(list);
        const int collapsedHeight = popup->height();
        QCOMPARE(check->parentWidget()->height(), 0);
        popup->activateWindow();
        filter->setFocus();
        QTest::keyClicks(filter, QStringLiteral("needle"));
        QTRY_VERIFY(check->parentWidget()->height() >= check->height());
        QVERIFY(check->mapTo(popup, QPoint()).y() < filter->mapTo(popup, QPoint()).y());
        QTRY_COMPARE(list->count(), 2);
        QCOMPARE(storage_->loads, 0);
        QTest::mouseClick(check, Qt::LeftButton);
        QVERIFY(check->isChecked());
        QTRY_COMPARE(list->count(), 3);
        QVERIFY(filter->hasFocus());
        QVERIFY(popup->height() > collapsedHeight);
        filter->clear();
        QTRY_COMPARE(list->count(), 4);
        QTRY_COMPARE(check->parentWidget()->height(), 0);
        QTRY_COMPARE(popup->height(), collapsedHeight);

        storage_->holdLoads = true;
        filter->setText(QStringLiteral("different"));
        QTRY_COMPARE(storage_->heldLoads.size(), 1);
        QSignalSpy cancelled(storage_->heldLoads.first(), &StorageJob::finished);
        popup->close();
        QTRY_VERIFY(!popup);
        QCOMPARE(cancelled.count(), 1);
    }

private:
    SearchStorage *storage_ { nullptr };
};

QTEST_MAIN(TrayNotesTest)

#include "traynotes_test.moc"
