#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

#include "desktopeditorplatformbackend.h"
#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "draftstore.h"
#include "noteblockmodel.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notesmanagerwindow.h"
#include "themediconimageprovider.h"

using namespace QtNote;

namespace {
class MemoryDraftStore final : public DraftStore {
public:
    DraftStoreError write(const DraftRecord &record) override
    {
        records_.insert(record.id, record);
        return {};
    }
    DraftStoreResult<DraftRecord> load(const QUuid &id) const override
    {
        const auto it = records_.constFind(id);
        if (it == records_.cend())
            return { {}, { DraftStoreError::NotFound, QStringLiteral("not found") } };
        return { it.value(), {} };
    }
    DraftStoreResult<QList<DraftRecord>> records() const override { return { records_.values(), {} }; }
    DraftStoreError                      transition(const QUuid &id, DraftRecord::State state) override
    {
        auto it = records_.find(id);
        if (it == records_.end())
            return { DraftStoreError::NotFound, QStringLiteral("not found") };
        it->state = state;
        return {};
    }
    DraftStoreError remove(const QUuid &id) override
    {
        return records_.remove(id) ? DraftStoreError {}
                                   : DraftStoreError { DraftStoreError::NotFound, QStringLiteral("not found") };
    }

private:
    QHash<QUuid, DraftRecord> records_;
};

Note plainNote()
{
    Note note(new NoteData(nullptr));
    note.setTitle(QStringLiteral("Title"));
    note.setText(QStringLiteral("Body"), Note::PlainText);
    return note;
}
}

class DesktopNoteEditorHostTest : public QObject {
    Q_OBJECT

private slots:
    void loadsSharedQmlShell()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        auto *quick = host.quickWidget();
        QVERIFY(quick);
        QCOMPARE(quick->status(), QQuickWidget::Ready);
        QVERIFY(quick->rootObject());
        QVariant viewState;
        QVERIFY(
            QMetaObject::invokeMethod(quick->rootObject(), "captureEditorState", Q_RETURN_ARG(QVariant, viewState)));
        QVERIFY(viewState.canConvert<QVariantMap>());
        QCOMPARE(host.model(), editor.model());
    }

    void loadsNotesManagerQmlShell()
    {
        DraftManager       drafts(std::make_unique<MemoryDraftStore>());
        NotesManagerWindow manager;
        QVERIFY(manager.isReady());
    }

    void notesManagerContextMenusAndInternalDragsWork()
    {
        QQuickWidget quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(620, 480);
        QStandardItemModel           notesModel;
        const QHash<int, QByteArray> roles {
            { Qt::UserRole + 1, "storageId" },    { Qt::UserRole + 2, "noteId" },
            { Qt::UserRole + 3, "itemType" },     { Qt::UserRole + 4, "title" },
            { Qt::UserRole + 5, "preview" },      { Qt::UserRole + 6, "loading" },
            { Qt::UserRole + 7, "errorString" },  { Qt::UserRole + 8, "hasMore" },
            { Qt::UserRole + 9, "noteCount" },    { Qt::UserRole + 10, "iconSource" },
            { Qt::UserRole + 11, "tags" },        { Qt::UserRole + 12, "modifiedTime" },
            { Qt::UserRole + 13, "storageName" }, { Qt::UserRole + 14, "accessible" },
        };
        notesModel.setItemRoleNames(roles);
        const auto appendItem = [&notesModel](const QString &storageId, const QString &noteId, int itemType,
                                              const QString &title) {
            auto *item = new QStandardItem;
            item->setData(storageId, Qt::UserRole + 1);
            item->setData(noteId, Qt::UserRole + 2);
            item->setData(itemType, Qt::UserRole + 3);
            item->setData(title, Qt::UserRole + 4);
            item->setData(QString(), Qt::UserRole + 5);
            item->setData(false, Qt::UserRole + 6);
            item->setData(QString(), Qt::UserRole + 7);
            item->setData(false, Qt::UserRole + 8);
            item->setData(itemType == 0 && storageId == QStringLiteral("storage-a") ? 1 : 0, Qt::UserRole + 9);
            item->setData(QString(), Qt::UserRole + 10);
            item->setData(QStringList(), Qt::UserRole + 11);
            item->setData(QDateTime(), Qt::UserRole + 12);
            item->setData(itemType == 0 ? title
                                        : (storageId == QStringLiteral("storage-a") ? QStringLiteral("Storage A")
                                                                                    : QStringLiteral("Storage B")),
                          Qt::UserRole + 13);
            item->setData(true, Qt::UserRole + 14);
            notesModel.appendRow(item);
        };
        appendItem(QStringLiteral("storage-a"), {}, 0, QStringLiteral("Storage A"));
        appendItem(QStringLiteral("storage-a"), QStringLiteral("note-a"), 1, QStringLiteral("Note A"));
        appendItem(QStringLiteral("storage-b"), {}, 0, QStringLiteral("Storage B"));
        installThemedIconImageProvider(quick.engine());
        quick.rootContext()->setContextProperty(QStringLiteral("testNotesModel"), &notesModel);
        QQmlComponent component(quick.engine());
        component.setData(R"QML(
            import QtQuick
            import QtQuick.Controls

            Item {
                id: harness
                objectName: "managerInteractionHarness"
                property int movedNotes: workspace.movedNotes
                property string noteDestination: workspace.noteDestination
                property int movedStorages: workspace.movedStorages
                property string storageDestination: workspace.storageDestination

                QtObject {
                    id: workspace
                    property var groupedNotesModel: testNotesModel
                    property var recentNotesModel: testNotesModel
                    property var currentEditor: null
                    property string currentStorageId: ""
                    property string currentNoteId: ""
                    property string currentTitle: ""
                    property string errorString: ""
                    property string searchText: ""
                    property bool searchInBody: false
                    property bool loading: false
                    property bool busy: false
                    property int noteCount: 1
                    property var storages: []
                    property int movedNotes: 0
                    property string noteDestination: ""
                    property int movedStorages: 0
                    property string storageDestination: ""
                    function saveCurrentNote() { return true }
                    function closeCurrentNote() { return true }
                    function reloadCurrentNote() { return true }
                    function openNote(storageId, noteId) { return true }
                    function createNote(storageId) { return true }
                    function openStandalone(storageId, noteId) { return true }
                    function deleteNote(storageId, noteId) { return true }
                    function copyNote(sourceStorageId, noteId, destinationStorageId) { return true }
                    function moveNote(sourceStorageId, noteId, destinationStorageId) { return true }
                    function openStorageSettings(storageId) {}
                    function moveNotes(notes, destinationStorageId) {
                        movedNotes = notes.length
                        noteDestination = destinationStorageId
                        return true
                    }
                    function moveStorage(sourceStorageId, destinationStorageId) {
                        ++movedStorages
                        storageDestination = destinationStorageId
                        return true
                    }
                }

                NotesManagerPage {
                    id: page
                    objectName: "managerPage"
                    anchors.fill: parent
                    workspace: workspace
                    embeddedEditor: false
                    showCreateButton: false
                    showViewModeSelector: false
                    viewMode: groupedByStorageMode
                }
            }
        )QML",
                          QUrl(QStringLiteral("qrc:/qml/ManagerInteractionHarness.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/ManagerInteractionHarness.qml")), &component, root);
        quick.show();
        QTest::qWait(50);

        const auto delegate = [](QQuickItem *page, int row) -> QQuickItem * {
            QVariant value;
            if (!QMetaObject::invokeMethod(page, "groupedItemAtRow", Q_RETURN_ARG(QVariant, value),
                                           Q_ARG(QVariant, row))) {
                return nullptr;
            }
            return qobject_cast<QQuickItem *>(value.value<QObject *>());
        };
        QQuickItem *storageA = nullptr;
        QQuickItem *noteA    = nullptr;
        QQuickItem *storageB = nullptr;
        auto       *page     = root->findChild<QQuickItem *>(QStringLiteral("managerPage"));
        auto       *tree     = root->findChild<QQuickItem *>(QStringLiteral("notesTree"));
        auto       *preview  = root->findChild<QQuickItem *>(QStringLiteral("managerDragPreview"));
        QVERIFY(page);
        QVERIFY(tree);
        QVERIFY(preview);
        QTRY_VERIFY((storageA = delegate(page, 0)));
        QTRY_VERIFY((noteA = delegate(page, 1)));
        QTRY_VERIFY((storageB = delegate(page, 2)));

        const QPointF storageAPoint = storageA->mapToItem(qobject_cast<QQuickItem *>(root),
                                                          QPointF(storageA->width() / 2, storageA->height() / 2));
        QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, storageAPoint.toPoint());
        QTRY_COMPARE(page->property("selectedStorageId").toString(), QStringLiteral("storage-a"));
        QTRY_VERIFY(root->findChild<QObject *>(QStringLiteral("storageContextMenu"))->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("storageContextMenu")), "close"));

        const QPointF noteAPoint
            = noteA->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(noteA->width() / 2, noteA->height() / 2));
        QTest::mouseClick(&quick, Qt::RightButton, Qt::NoModifier, noteAPoint.toPoint());
        QTRY_COMPARE(page->property("selectedNoteId").toString(), QStringLiteral("note-a"));
        QTRY_VERIFY(root->findChild<QObject *>(QStringLiteral("noteContextMenu"))->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(root->findChild<QObject *>(QStringLiteral("noteContextMenu")), "close"));

        const auto drag = [&quick, root, preview](QQuickItem *source, QQuickItem *destination, int previewItems) {
            auto         *rootItem = qobject_cast<QQuickItem *>(root);
            const QPointF from     = source->mapToItem(rootItem, QPointF(source->width() / 2, source->height() / 2));
            const QPointF to
                = destination->mapToItem(rootItem, QPointF(destination->width() / 2, destination->height() / 2));
            QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            for (int step = 1; step <= 8; ++step)
                QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
            QTRY_COMPARE(preview->property("previewCount").toInt(), previewItems);
            QTRY_VERIFY(destination->property("dragHovered").toBool());
            QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
            QTRY_COMPARE(preview->property("previewCount").toInt(), 0);
        };

        drag(noteA, storageB, 1);
        QTRY_COMPARE(root->property("movedNotes").toInt(), 1);
        QCOMPARE(root->property("noteDestination").toString(), QStringLiteral("storage-b"));

        drag(storageA, storageB, 2);
        QTRY_COMPARE(root->property("movedStorages").toInt(), 1);
        QCOMPARE(root->property("storageDestination").toString(), QStringLiteral("storage-b"));
    }

    void customSpellingDictionaryPersistsAndCanBeEdited()
    {
        QSettings      settings;
        const QString  key      = QStringLiteral("editor/customSpellingDictionary");
        const QVariant previous = settings.value(key);

        {
            DesktopEditorPlatformBackend backend;
            backend.setCustomSpellingDictionary(
                { QStringLiteral("Beta"), QStringLiteral("alpha"), QStringLiteral("ALPHA"), QStringLiteral(" ") });
            const auto words = backend.customSpellingDictionary();
            QCOMPARE(words.size(), 2);
            QVERIFY(words.contains(QStringLiteral("alpha")));
            QVERIFY(words.contains(QStringLiteral("Beta")));
        }
        {
            DesktopEditorPlatformBackend backend;
            const auto                   words = backend.customSpellingDictionary();
            QCOMPARE(words.size(), 2);
            QVERIFY(words.contains(QStringLiteral("alpha")));
            QVERIFY(words.contains(QStringLiteral("Beta")));
        }

        if (previous.isValid())
            settings.setValue(key, previous);
        else
            settings.remove(key);
    }

    void modelAndControllerStayShared()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.model()->setBlockText(0, QStringLiteral("Changed\nBody"));
        QCOMPARE(editor.text(), QStringLiteral("Changed\nBody"));
        QVERIFY(editor.isDirty());

        editor.setMarkdown(true);
        QVERIFY(editor.isMarkdown());
        QVERIFY(host.model()->markdown());
    }

    void structuralCommandsUseTheCommonHistory()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.setMarkdown(true);
        const int before = editor.model()->rowCount();
        host.insertTable();
        QCoreApplication::processEvents();
        QCOMPARE(editor.model()->rowCount(), before + 1);
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->rowCount(), before);
    }

    void focusAdapterEmitsCheckpointSignal()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);
        QSignalSpy            lost(&host, &DesktopNoteEditorHost::focusLost);

        host.show();
        QTest::qWait(20);
        QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &event);
        QCOMPARE(lost.size(), 1);
    }
};

QTEST_MAIN(DesktopNoteEditorHostTest)

#include "desktopnoteeditorhost_test.moc"
