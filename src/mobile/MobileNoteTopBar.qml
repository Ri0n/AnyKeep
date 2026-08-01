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

    implicitHeight: 52

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        spacing: 2

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            text: "‹"
            font.pixelSize: 30
            padding: 0
            Accessible.name: qsTr("Back")
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
            text: "↶"
            font.pixelSize: 23
            enabled: root.editorBackend && root.editorBackend.canUndo
            Accessible.name: root.editorBackend && root.editorBackend.undoText.length > 0
                             ? qsTr("Undo %1").arg(root.editorBackend.undoText) : qsTr("Undo")
            onClicked: root.editorBackend.undo()
        }

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            text: "↷"
            font.pixelSize: 23
            enabled: root.editorBackend && root.editorBackend.canRedo
            Accessible.name: root.editorBackend && root.editorBackend.redoText.length > 0
                             ? qsTr("Redo %1").arg(root.editorBackend.redoText) : qsTr("Redo")
            onClicked: root.editorBackend.redo()
        }

        ToolButton {
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            text: "⋮"
            font.pixelSize: 26
            Accessible.name: qsTr("More note actions")
            onClicked: noteMenu.popup()

            Menu {
                id: noteMenu
                y: parent.height

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
