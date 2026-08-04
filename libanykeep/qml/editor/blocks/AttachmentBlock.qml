import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FocusScope {
    id: attachmentRoot
    required property var editorView
    objectName: "attachmentBlockEditor-" + block.index
    required property var block
    property bool selected: attachmentRoot.editorView.selectedAttachmentIndex === block.index
    width: block.width
    implicitHeight: attachmentCard.implicitHeight
    activeFocusOnTab: true

    function selectAndFocus() {
        attachmentRoot.editorView.selectAttachmentBlock(block.index)
        attachmentRoot.forceActiveFocus()
    }

    function formatSize(bytes) {
        const value = Math.max(0, Number(bytes))
        if (value < 1024)
            return qsTr("%1 B").arg(value)
        if (value < 1024 * 1024)
            return qsTr("%1 KB").arg((value / 1024).toFixed(value < 10 * 1024 ? 1 : 0))
        if (value < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((value / (1024 * 1024)).toFixed(value < 10 * 1024 * 1024 ? 1 : 0))
        return qsTr("%1 GB").arg((value / (1024 * 1024 * 1024)).toFixed(1))
    }

    Keys.onPressed: function(event) {
        if (event.matches(StandardKey.Copy)) {
            event.accepted = attachmentRoot.editorView.copyActiveSelection()
            return
        }
        if (event.modifiers)
            return
        if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            attachmentRoot.editorView.removeAttachmentBlock(block.index, true)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (attachmentRoot.editorView.platformBackend
                    && typeof attachmentRoot.editorView.platformBackend.openAttachment === "function")
                attachmentRoot.editorView.platformBackend.openAttachment(block.url)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            attachmentRoot.editorView.clearAttachmentSelection()
            attachmentRoot.editorView.forceActiveFocus()
            event.accepted = true
        }
    }

    Rectangle {
        id: attachmentCard
        width: parent.width
        implicitHeight: attachmentRoot.editorView.touchMode ? 64 : 54
        radius: 6
        color: attachmentName.palette.base
        border.width: attachmentRoot.selected ? 2 : 1
        border.color: attachmentRoot.selected
                      ? attachmentName.palette.highlight : attachmentName.palette.mid

        RowLayout {
            anchors.fill: parent
            anchors.margins: attachmentRoot.editorView.touchMode ? 8 : 6
            spacing: 10

            Label {
                Layout.preferredWidth: attachmentRoot.editorView.touchMode ? 42 : 34
                horizontalAlignment: Text.AlignHCenter
                text: "📎"
                font.pixelSize: attachmentRoot.editorView.touchMode ? 23 : 19
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    id: attachmentName
                    Layout.fillWidth: true
                    text: attachmentRoot.block.alt.length > 0
                          ? attachmentRoot.block.alt : qsTr("Attached file")
                    elide: Text.ElideMiddle
                    font.bold: attachmentRoot.selected
                }
                Label {
                    Layout.fillWidth: true
                    text: {
                        const details = []
                        if (attachmentRoot.block.attachmentMediaType.length > 0)
                            details.push(attachmentRoot.block.attachmentMediaType)
                        if (attachmentRoot.block.attachmentSize > 0)
                            details.push(attachmentRoot.formatSize(attachmentRoot.block.attachmentSize))
                        return details.join("  ·  ")
                    }
                    color: attachmentName.palette.placeholderText
                    elide: Text.ElideRight
                    font.pixelSize: Math.max(10, attachmentRoot.editorView.editorFontMetricsHeight * 0.65)
                }
            }

            ToolButton {
                Layout.preferredWidth: attachmentRoot.editorView.touchMode ? 42 : 34
                Layout.preferredHeight: Layout.preferredWidth
                visible: attachmentRoot.editorView.platformBackend
                         && typeof attachmentRoot.editorView.platformBackend.openAttachment === "function"
                text: "↗"
                Accessible.name: qsTr("Open attached file")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    attachmentRoot.selectAndFocus()
                    attachmentRoot.editorView.platformBackend.openAttachment(attachmentRoot.block.url)
                }
            }

            ToolButton {
                Layout.preferredWidth: attachmentRoot.editorView.touchMode ? 40 : 30
                Layout.preferredHeight: Layout.preferredWidth
                visible: attachmentRoot.selected
                text: "×"
                Accessible.name: qsTr("Remove attached file")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: attachmentRoot.editorView.removeAttachmentBlock(attachmentRoot.block.index, true)
            }
        }

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: attachmentRoot.selectAndFocus()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: {
                attachmentRoot.selectAndFocus()
                attachmentMenu.popup()
            }
        }
    }

    Menu {
        id: attachmentMenu
        MenuItem {
            text: qsTr("Open")
            visible: attachmentRoot.editorView.platformBackend
                     && typeof attachmentRoot.editorView.platformBackend.openAttachment === "function"
            onTriggered: attachmentRoot.editorView.platformBackend.openAttachment(attachmentRoot.block.url)
        }
        MenuItem {
            text: qsTr("Save a copy")
            visible: attachmentRoot.editorView.platformBackend
                     && typeof attachmentRoot.editorView.platformBackend.saveAttachmentAs === "function"
            onTriggered: attachmentRoot.editorView.platformBackend.saveAttachmentAs(attachmentRoot.block.url)
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Remove Attached File")
            onTriggered: attachmentRoot.editorView.removeAttachmentBlock(attachmentRoot.block.index, true)
        }
    }
}
