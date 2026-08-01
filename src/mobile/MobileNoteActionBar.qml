import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    required property var editorBackend
    required property var actions
    property bool voiceAvailable: false
    property bool voiceBusy: false
    property bool voiceRecording: false
    property bool voiceModeSwitchVisible: false
    property int voiceMode: 1
    property string voiceStatus: ""

    signal voiceRequested()
    signal voiceCancelRequested()
    signal voiceModeRequested(int mode)

    implicitHeight: 58

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
        visible: !root.voiceRecording && !root.voiceBusy

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
            text: qsTr("Image")
            enabled: root.editorBackend && root.editorBackend.supportsMedia
            Accessible.name: qsTr("Insert image or take photo")
            themeName: "insert-image-symbolic"
            fallbackName: "insert-image-symbolic.svg"
            onClicked: imageMenu.popup()

            Menu {
                id: imageMenu
                y: -implicitHeight
                MenuItem { text: qsTr("Choose image"); onTriggered: root.actions.insertImage() }
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
            onClicked: insertMenu.popup()

            Menu {
                id: insertMenu
                y: -implicitHeight

                Menu {
                    title: qsTr("Voice button")
                    visible: root.voiceModeSwitchVisible
                    enabled: visible
                    MenuItem {
                        text: qsTr("Audio recording")
                        checkable: true
                        checked: root.voiceMode === 1
                        onTriggered: root.voiceModeRequested(1)
                    }
                    MenuItem {
                        text: qsTr("Speech to text")
                        checkable: true
                        checked: root.voiceMode === 0
                        onTriggered: root.voiceModeRequested(0)
                    }
                }
                MenuSeparator { visible: root.voiceModeSwitchVisible }
                MenuItem { text: qsTr("Bullet list"); onTriggered: root.actions.insertList(root.actions.bulletListType) }
                MenuItem { text: qsTr("Numbered list"); onTriggered: root.actions.insertList(root.actions.numberedListType) }
                MenuItem { text: qsTr("Block quote"); onTriggered: root.actions.insertBlockQuote() }
                MenuItem { text: qsTr("Code block"); onTriggered: root.actions.insertCodeBlock() }
                MenuItem { text: qsTr("Table"); onTriggered: root.actions.insertTable() }
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
