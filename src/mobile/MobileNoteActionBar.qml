import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    required property var editorBackend
    required property var actions
    readonly property var blockEditor: actions.blockEditor
    readonly property bool selectionActive: blockEditor && blockEditor.touchSelectionActive
    property bool voiceAvailable: false
    property bool voiceBusy: false
    property bool voiceRecording: false
    property bool voiceModeSwitchVisible: false
    property int voiceMode: 1
    property string voiceStatus: ""

    signal voiceRequested()
    signal voiceCancelRequested()
    signal voiceModeRequested(int mode)

    function openInsertMenu(button) {
        if (!Overlay.overlay) {
            insertMenu.open()
            return
        }
        const point = button.mapToItem(Overlay.overlay, button.width, 0)
        const inset = 8
        const menuWidth = Math.max(insertMenu.width, insertMenu.implicitWidth)
        const menuHeight = Math.max(insertMenu.height, insertMenu.implicitHeight)
        const x = Math.max(inset, Math.min(Overlay.overlay.width - menuWidth - inset,
                                           point.x - menuWidth))
        const y = Math.max(inset, point.y - menuHeight - 6)
        insertMenu.popup(Overlay.overlay, x, y)
    }

    implicitHeight: 58
    background: Rectangle {
        color: root.palette.window
    }

    component ActionButton: ToolButton {
        id: actionButton
        property string themeName
        property string fallbackName
        Layout.fillWidth: true
        Layout.preferredHeight: 52
        padding: 2

        contentItem: ColumnLayout {
            spacing: 1

            ThemedIcon {
                Layout.alignment: Qt.AlignHCenter
                themeName: actionButton.themeName
                fallbackName: actionButton.fallbackName
                recolorFallback: true
                pixelSize: 22
                enabled: actionButton.enabled
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: actionButton.text
                elide: Text.ElideRight
                font.pixelSize: 10
                color: actionButton.palette.buttonText
                opacity: actionButton.enabled ? 1 : 0.38
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 2
        anchors.rightMargin: 2
        spacing: 0
        visible: !root.voiceRecording && !root.voiceBusy && !root.selectionActive

        ActionButton {
            text: qsTr("File")
            enabled: root.editorBackend && root.editorBackend.supportsMedia
            Accessible.name: qsTr("Attach file")
            themeName: "mail-attachment-symbolic"
            fallbackName: "attachment-symbolic.svg"
            onClicked: root.actions.insertAttachment()
        }
        ActionButton {
            text: qsTr("Tasks")
            Accessible.name: qsTr("Insert task list")
            themeName: "view-task-symbolic"
            fallbackName: "task-list-symbolic.svg"
            onClicked: root.actions.insertList(root.actions.taskListType)
        }
        ActionButton {
            text: root.voiceMode === 1 ? qsTr("Record") : qsTr("Dictate")
            enabled: root.voiceAvailable
            Accessible.name: root.voiceMode === 1 ? qsTr("Record audio") : qsTr("Speech to text")
            themeName: root.voiceMode === 1 ? "media-record-symbolic" : "audio-input-microphone-symbolic"
            fallbackName: root.voiceMode === 1 ? "media-record-symbolic.svg" : "microphone.svg"
            onClicked: root.voiceRequested()
        }
        ActionButton {
            text: qsTr("Media")
            enabled: root.editorBackend && root.editorBackend.supportsMedia
            Accessible.name: qsTr("Insert media or take photo")
            themeName: "insert-image-symbolic"
            fallbackName: "insert-image-symbolic.svg"
            onClicked: imageMenu.popup()

            Menu {
                id: imageMenu
                y: -implicitHeight
                MenuItem { text: qsTr("Choose media"); onTriggered: root.actions.insertMedia() }
                MenuItem {
                    text: qsTr("Take photo")
                    visible: root.actions.platformBackend
                             && typeof root.actions.platformBackend.insertPhoto === "function"
                    height: visible ? implicitHeight : 0
                    onTriggered: root.actions.insertPhoto()
                }
            }
        }
        ActionButton {
            text: qsTr("More")
            Accessible.name: qsTr("More insert actions")
            themeName: "overflow-menu-symbolic"
            fallbackName: "overflow-menu-symbolic.svg"
            onClicked: root.openInsertMenu(this)

            Menu {
                id: insertMenu
                parent: Overlay.overlay

                MenuItem {
                    text: qsTr("Voice input: Audio recording")
                    visible: root.voiceModeSwitchVisible
                    height: visible ? implicitHeight : 0
                    checkable: true
                    checked: root.voiceMode === 1
                    onTriggered: root.voiceModeRequested(1)
                }
                MenuItem {
                    text: qsTr("Voice input: Speech to text")
                    visible: root.voiceModeSwitchVisible
                    height: visible ? implicitHeight : 0
                    checkable: true
                    checked: root.voiceMode === 0
                    onTriggered: root.voiceModeRequested(0)
                }
                MenuItem { text: qsTr("Bullet list"); onTriggered: root.actions.insertList(root.actions.bulletListType) }
                MenuItem { text: qsTr("Numbered list"); onTriggered: root.actions.insertList(root.actions.numberedListType) }
                MenuItem { text: qsTr("Block quote"); onTriggered: root.actions.insertBlockQuote() }
                MenuItem { text: qsTr("Code block"); onTriggered: root.actions.insertCodeBlock() }
                MenuItem { text: qsTr("Table"); onTriggered: root.actions.insertTable() }
                MenuItem {
                    text: qsTr("Paste")
                    enabled: root.blockEditor.activeEditor && root.blockEditor.activeEditor.canPaste
                    onTriggered: root.blockEditor.pasteClipboard()
                }
                MenuItem { text: qsTr("Select all"); onTriggered: root.blockEditor.selectAllDocument() }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 2
        visible: root.selectionActive && !root.voiceRecording && !root.voiceBusy

        ToolButton {
            Layout.fillWidth: true
            Layout.fillHeight: true
            focusPolicy: Qt.NoFocus
            text: "B"
            font.bold: true
            font.pixelSize: 20
            Accessible.name: qsTr("Bold")
            enabled: root.editorBackend.markdown
            onClicked: root.blockEditor.applyInlineFormattingStyle("bold", root.blockEditor.activeEditor)
        }
        ToolButton {
            Layout.fillWidth: true
            Layout.fillHeight: true
            focusPolicy: Qt.NoFocus
            text: "I"
            font.italic: true
            font.pixelSize: 20
            Accessible.name: qsTr("Italic")
            enabled: root.editorBackend.markdown
            onClicked: root.blockEditor.applyInlineFormattingStyle("italic", root.blockEditor.activeEditor)
        }
        ToolButton {
            Layout.fillWidth: true
            Layout.fillHeight: true
            focusPolicy: Qt.NoFocus
            text: "S"
            font.strikeout: true
            font.pixelSize: 20
            Accessible.name: qsTr("Strikethrough")
            enabled: root.editorBackend.markdown
            onClicked: root.blockEditor.applyInlineFormattingStyle("strike", root.blockEditor.activeEditor)
        }
        ActionButton {
            text: qsTr("Copy")
            focusPolicy: Qt.NoFocus
            themeName: "__bundled__"
            fallbackName: "edit-copy-symbolic.svg"
            onClicked: root.blockEditor.copyDocumentSelection()
        }
        ActionButton {
            text: qsTr("More")
            focusPolicy: Qt.NoFocus
            Accessible.name: qsTr("Selection actions")
            themeName: "__bundled__"
            fallbackName: "overflow-menu-symbolic.svg"
            onClicked: selectionMenu.open()

            Menu {
                id: selectionMenu
                y: -height
                margins: 8
                MenuItem {
                    text: qsTr("Cut")
                    enabled: root.blockEditor.touchSelectionCanCut
                    onTriggered: root.blockEditor.cutDocumentSelection()
                }
                MenuItem {
                    text: qsTr("Paste")
                    enabled: !root.blockEditor.selectionSpansEditors
                             && root.blockEditor.activeEditor && root.blockEditor.activeEditor.canPaste
                    onTriggered: root.blockEditor.pasteClipboard()
                }
                MenuItem { text: qsTr("Select all"); onTriggered: root.blockEditor.selectAllDocument() }
                MenuItem { text: qsTr("Clear selection"); onTriggered: root.blockEditor.clearDocumentSelection() }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8
        visible: root.voiceRecording || root.voiceBusy

        ToolButton {
            Layout.preferredWidth: 48
            Layout.preferredHeight: 48
            text: qsTr("Cancel")
            enabled: !root.voiceBusy || root.voiceRecording
            onClicked: root.voiceCancelRequested()
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.voiceStatus.length > 0 ? root.voiceStatus
                                               : (root.voiceRecording ? qsTr("Recording…") : qsTr("Saving…"))
        }
        ToolButton {
            Layout.preferredWidth: 48
            Layout.preferredHeight: 48
            text: root.voiceRecording ? "■" : "…"
            enabled: root.voiceRecording
            Accessible.name: qsTr("Stop recording")
            onClicked: root.voiceRequested()
        }
    }
}
