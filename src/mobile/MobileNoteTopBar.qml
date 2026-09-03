import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    required property var editorBackend
    required property var actions
    property bool shortcutVisible: false

    signal backRequested()
    signal shareRequested()
    signal exportRequested()
    signal findRequested()
    signal deleteRequested()
    signal addToHomeScreenRequested()

    function openNoteMenu(button) {
        if (!Overlay.overlay) {
            noteMenu.open()
            return
        }
        const point = button.mapToItem(Overlay.overlay, button.width, button.height)
        const inset = 8
        const menuWidth = Math.max(noteMenu.width, noteMenu.implicitWidth)
        const menuHeight = Math.max(noteMenu.height, noteMenu.implicitHeight)
        const x = Math.max(inset, Math.min(Overlay.overlay.width - menuWidth - inset,
                                           point.x - menuWidth))
        const y = Math.max(inset, Math.min(Overlay.overlay.height - menuHeight - inset,
                                           point.y + 6))
        noteMenu.popup(Overlay.overlay, x, y)
    }

    implicitHeight: 52
    background: Rectangle {
        color: root.palette.window
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        spacing: 2

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            padding: 0
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Back")
            contentItem: ThemedIcon {
                themeName: "__bundled__"
                fallbackName: "go-next-symbolic.svg"
                recolorFallback: true
                pixelSize: 22
                rotation: 180
            }
            onClicked: root.backRequested()
        }

        Item { Layout.fillWidth: true }

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("Share")
            contentItem: ThemedIcon {
                themeName: "document-share-symbolic"
                fallbackName: "document-share-symbolic.svg"
                recolorFallback: true
                pixelSize: 22
            }
            onClicked: root.shareRequested()
        }

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            padding: 0
            display: AbstractButton.IconOnly
            enabled: root.editorBackend && root.editorBackend.canUndo
            Accessible.name: root.editorBackend && root.editorBackend.undoText.length > 0
                             ? qsTr("Undo %1").arg(root.editorBackend.undoText) : qsTr("Undo")
            contentItem: ThemedIcon {
                themeName: "__bundled__"
                fallbackName: "edit-undo-symbolic.svg"
                recolorFallback: true
                pixelSize: 22
            }
            onClicked: root.editorBackend.undo()
        }

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            padding: 0
            display: AbstractButton.IconOnly
            enabled: root.editorBackend && root.editorBackend.canRedo
            Accessible.name: root.editorBackend && root.editorBackend.redoText.length > 0
                             ? qsTr("Redo %1").arg(root.editorBackend.redoText) : qsTr("Redo")
            contentItem: ThemedIcon {
                themeName: "__bundled__"
                fallbackName: "edit-redo-symbolic.svg"
                recolorFallback: true
                pixelSize: 22
            }
            onClicked: root.editorBackend.redo()
        }

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            padding: 0
            display: AbstractButton.IconOnly
            Accessible.name: qsTr("More note actions")
            contentItem: ThemedIcon {
                themeName: "__bundled__"
                fallbackName: "overflow-menu-symbolic.svg"
                recolorFallback: true
                pixelSize: 22
            }
            onClicked: root.openNoteMenu(this)

            Menu {
                id: noteMenu
                parent: Overlay.overlay

                MenuItem { text: qsTr("Find in note"); onTriggered: root.findRequested() }
                MenuItem { text: qsTr("Copy note"); onTriggered: root.actions.copyDocument() }
                MenuItem { text: qsTr("Export"); onTriggered: root.exportRequested() }
                MenuItem {
                    visible: root.shortcutVisible
                    height: visible ? implicitHeight : 0
                    text: qsTr("Add to Home screen")
                    onTriggered: root.addToHomeScreenRequested()
                }
                MenuItem {
                    text: root.editorBackend && root.editorBackend.markdown
                          ? qsTr("Switch to plain text") : qsTr("Switch to Markdown")
                    onTriggered: root.actions.toggleMarkdownMode()
                }
                MenuSeparator { }
                MenuItem {
                    text: qsTr("Delete note")
                    onTriggered: root.deleteRequested()
                }
            }
        }
    }
}
