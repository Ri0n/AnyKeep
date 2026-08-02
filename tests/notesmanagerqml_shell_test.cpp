#include <QElapsedTimer>
#include <QJsonDocument>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteeditor.h"
#include "notesmanagerwindow.h"
#include "themediconimageprovider.h"

#include "desktopqmltestsupport.h"
#include "editortestsupport.h"
#include "notesmanagerqml_test.h"
#include "quicktestsupport.h"

using namespace QtNote;
using namespace QtNote::TestSupport;

void NotesManagerQmlTest::loadsNotesManagerQmlShell()
{
    DraftManager       drafts(std::make_unique<MemoryDraftStore>());
    NotesManagerWindow manager;
    QVERIFY(manager.isReady());
}

void NotesManagerQmlTest::notesManagerFoldersTabInstantiatesInOwnerContext()
{
    FolderPageTestModel foldersModel;
    QQuickWidget        quick;
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(420, 430);
    installThemedIconImageProvider(quick.engine());
    quick.rootContext()->setContextProperty(QStringLiteral("testFoldersModel"), &foldersModel);

    QQmlComponent component(quick.engine());
    component.setData(R"QML(
        import QtQuick
        import QtQuick.Controls

        Item {
            id: harness

            QtObject {
                id: workspace
                property var groupedNotesModel: null
                property var recentNotesModel: null
                property var folderNotesModel: testFoldersModel
                property bool folderCatalogAvailable: true
                property var currentEditor: null
                property string currentStorageId: ""
                property string currentNoteId: ""
                property string currentTitle: ""
                property string errorString: ""
                property string searchText: ""
                property bool searchInBody: false
                property bool loading: false
                property bool busy: false
                property int noteCount: 3
                property var storages: []

                function saveCurrentNote() { return true }
                function closeCurrentNote() { return true }
                function reloadCurrentNote() { return true }
                function openNote(storageId, noteId) { return true }
                function createNote(storageId) { return true }
                function createNoteInFolder(folderId, storageId) { return true }
                function createFolder(name, parentFolderId) { return "" }
                function renameFolder(folderId, name) { return true }
                function moveFolderBefore(folderId, parentFolderId, beforeFolderId) { return true }
                function setFolderCollapsed(folderId, collapsed) { return true }
                function setFolderFlags(folderId, favorite, archived) { return true }
                function collapseAllFolders() { return true }
                function folderIdForNote(storageId, noteId) { return "" }
                function assignNoteFolder(storageId, noteId, folderId) { return true }
                function openStandalone(storageId, noteId) { return true }
                function deleteNote(storageId, noteId) { return true }
                function trashNote(storageId, noteId) { return true }
                function restoreRecycledNote(storageId, noteId) { return true }
                function isRecycledNote(storageId, noteId) { return false }
                function copyNote(sourceStorageId, noteId, destinationStorageId) { return true }
                function moveNote(sourceStorageId, noteId, destinationStorageId) { return true }
                function moveNotes(notes, destinationStorageId, anchorNoteId, insertAfter) { return true }
                function moveStorage(sourceStorageId, destinationStorageId) { return true }
                function moveStorageToRow(sourceStorageId, destinationRow) { return true }
                function openStorageSettings(storageId) {}
            }

            NotesManagerPage {
                id: page
                objectName: "managerFoldersPage"
                anchors.fill: parent
                workspace: workspace
                embeddedEditor: false
                showCreateButton: false
                showViewModeSelector: false
                viewMode: foldersMode
            }
        }
    )QML",
                      QUrl(QStringLiteral("qrc:/qml/ManagerFoldersHarness.qml")));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    quick.setContent(QUrl(QStringLiteral("qrc:/qml/ManagerFoldersHarness.qml")), &component, root.release());
    quick.show();

    auto *page = quick.rootObject()->findChild<QQuickItem *>(QStringLiteral("managerFoldersPage"));
    QVERIFY(page);
    QQuickItem *foldersList = nullptr;
    QTRY_VERIFY((foldersList = quickItemByName(page, QStringLiteral("foldersList"))));
    QVERIFY(foldersList->isVisible());
    auto *folderPicker = page->findChild<QObject *>(QStringLiteral("noteFolderPicker"));
    QVERIFY(folderPicker);
    QVERIFY(folderPicker->property("enabled").toBool());
    QVERIFY(!folderPicker->property("visible").toBool());
}
