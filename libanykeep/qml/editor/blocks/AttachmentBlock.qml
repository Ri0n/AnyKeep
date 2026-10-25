import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../shared" as Shared

FocusScope {
    id: attachmentRoot
    required property var editorView
    objectName: "attachmentBlockEditor-" + block.index
    required property var block
    property bool selected: attachmentRoot.editorView.selectedAttachmentIndex === block.index
    readonly property var backend: attachmentRoot.editorView.platformBackend
    readonly property var mimePresentation:
        backend && typeof backend.attachmentMimePresentation === "function"
        ? backend.attachmentMimePresentation(block.attachmentMediaType, block.alt)
        : ({ description: qsTr("File"), iconName: "", genericIconName: "application-x-generic" })
    readonly property bool canOpen: !!backend && typeof backend.openAttachment === "function"
    readonly property bool canSave: !!backend && typeof backend.saveAttachmentAs === "function"
    // A file kept in place already exists unencrypted, so it is shown in its
    // folder instead of being saved; encrypted and remote files are saved.
    property int storageRevision: 0
    readonly property bool keptInPlace: storageRevision >= 0 && !!backend
                                        && typeof backend.isAttachmentKeptInPlace === "function"
                                        && typeof backend.showAttachmentInFolder === "function"
                                        && backend.isAttachmentKeptInPlace(block.url)
    readonly property int actionButtonSize: attachmentRoot.editorView.touchMode ? 42 : 32
    width: block.width
    implicitHeight: attachmentCard.implicitHeight
    activeFocusOnTab: true

    Connections {
        target: attachmentRoot.backend
        ignoreUnknownSignals: true
        function onAttachmentStorageChanged(url) {
            if (url === attachmentRoot.block.url)
                ++attachmentRoot.storageRevision
        }
    }

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
            if (attachmentRoot.canOpen)
                attachmentRoot.backend.openAttachment(block.url)
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
        height: implicitHeight
        radius: 6
        color: attachmentName.palette.base
        border.width: attachmentRoot.selected ? 2 : 1
        border.color: attachmentRoot.selected
                      ? attachmentName.palette.highlight : attachmentName.palette.mid

        RowLayout {
            anchors.fill: parent
            anchors.margins: attachmentRoot.editorView.touchMode ? 8 : 6
            spacing: 10

            Item {
                Layout.preferredWidth: attachmentRoot.editorView.touchMode ? 42 : 34
                Layout.preferredHeight: Layout.preferredWidth
                Shared.ThemedIconImpl {
                    objectName: "attachmentFileGlyph-" + attachmentRoot.block.index
                    anchors.centerIn: parent
                    themeName: attachmentRoot.mimePresentation.iconName || "application-x-generic"
                    genericThemeName: attachmentRoot.mimePresentation.genericIconName || "application-x-generic"
                    fallbackName: "file-generic-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(attachmentName.palette.text)
                    pixelSize: attachmentRoot.editorView.touchMode ? 24 : 20
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    id: attachmentName
                    objectName: "attachmentFileName-" + attachmentRoot.block.index
                    Layout.fillWidth: true
                    text: attachmentRoot.block.alt.length > 0
                          ? attachmentRoot.block.alt : qsTr("Attached file")
                    elide: Text.ElideMiddle
                    font.bold: attachmentRoot.selected
                }
                Label {
                    objectName: "attachmentFileDetails-" + attachmentRoot.block.index
                    Layout.fillWidth: true
                    text: {
                        const details = []
                        if (attachmentRoot.mimePresentation.description)
                            details.push(attachmentRoot.mimePresentation.description)
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
                id: openButton
                objectName: "attachmentOpenButton-" + attachmentRoot.block.index
                Layout.preferredWidth: attachmentRoot.actionButtonSize
                Layout.preferredHeight: Layout.preferredWidth
                visible: attachmentRoot.canOpen
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "attachmentOpenGlyph-" + attachmentRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "open-external-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(openButton.palette.buttonText)
                    pixelSize: attachmentRoot.editorView.touchMode ? 22 : 18
                }
                Accessible.name: qsTr("Open attached file")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    attachmentRoot.selectAndFocus()
                    attachmentRoot.backend.openAttachment(attachmentRoot.block.url)
                }
            }

            ToolButton {
                id: saveButton
                objectName: "attachmentSaveButton-" + attachmentRoot.block.index
                Layout.preferredWidth: attachmentRoot.actionButtonSize
                Layout.preferredHeight: Layout.preferredWidth
                visible: attachmentRoot.canSave && !attachmentRoot.keptInPlace
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "attachmentSaveGlyph-" + attachmentRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "document-save-as-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(saveButton.palette.buttonText)
                    pixelSize: attachmentRoot.editorView.touchMode ? 22 : 18
                }
                Accessible.name: qsTr("Save attached file to…")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    attachmentRoot.selectAndFocus()
                    attachmentRoot.backend.saveAttachmentAs(attachmentRoot.block.url)
                }
            }

            ToolButton {
                id: showInFolderButton
                objectName: "attachmentShowInFolderButton-" + attachmentRoot.block.index
                Layout.preferredWidth: attachmentRoot.actionButtonSize
                Layout.preferredHeight: Layout.preferredWidth
                visible: attachmentRoot.keptInPlace
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "attachmentShowInFolderGlyph-" + attachmentRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "folder-open-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(showInFolderButton.palette.buttonText)
                    pixelSize: attachmentRoot.editorView.touchMode ? 22 : 18
                }
                Accessible.name: qsTr("Show attached file in folder")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    attachmentRoot.selectAndFocus()
                    attachmentRoot.backend.showAttachmentInFolder(attachmentRoot.block.url)
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
            visible: attachmentRoot.canOpen
            onTriggered: attachmentRoot.backend.openAttachment(attachmentRoot.block.url)
        }
        MenuItem {
            text: qsTr("Save To…")
            visible: attachmentRoot.canSave
            onTriggered: attachmentRoot.backend.saveAttachmentAs(attachmentRoot.block.url)
        }
        MenuItem {
            text: qsTr("Show in Folder")
            visible: attachmentRoot.keptInPlace
            onTriggered: attachmentRoot.backend.showAttachmentInFolder(attachmentRoot.block.url)
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Remove Attached File")
            onTriggered: attachmentRoot.editorView.removeAttachmentBlock(attachmentRoot.block.index, true)
        }
    }
}
