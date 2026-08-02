pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flickable {
    id: root

    required property var controller
    readonly property bool scrollBarAtWindowEdge: true
    readonly property int verticalScrollBarInset:
        contentHeight > height
        ? Math.ceil(Math.max(verticalScrollBar.width, verticalScrollBar.implicitWidth)) : 0
    readonly property int contentRightPadding: verticalScrollBarInset > 0 ? 8 : 12
    contentWidth: Math.max(0, width - verticalScrollBarInset - contentRightPadding)
    contentHeight: fieldsColumn.implicitHeight + 24
    clip: true

    function stringValue(value) {
        return value === undefined || value === null ? "" : String(value)
    }

    ScrollBar.vertical: ScrollBar { id: verticalScrollBar }

    Dialog {
        id: resetUsageDialog

        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: Math.min(420, parent.width - 48)
        modal: true
        title: qsTr("Reset usage statistics?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.controller.resetUsageStats()

        contentItem: Label {
            text: qsTr("This clears QtNote’s local SpeechKit counters only. Yandex Cloud billing history and charges are not changed.")
            wrapMode: Text.WordWrap
        }
    }

    ColumnLayout {
        id: fieldsColumn
        width: root.contentWidth
        spacing: 12

        Repeater {
            model: root.controller

            delegate: ColumnLayout {
                id: fieldDelegate

                required property int index
                required property string key
                required property string label
                required property string description
                required property int fieldType
                required property var fieldValue
                required property var options
                required property int minimum
                required property int maximum
                required property string placeholder
                required property bool restartRequired

                Layout.fillWidth: true
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    text: fieldDelegate.label
                    font.bold: true
                    wrapMode: Text.WordWrap
                }

                TextField {
                    Layout.fillWidth: true
                    visible: fieldDelegate.fieldType === 0 || fieldDelegate.fieldType === 1
                    objectName: visible ? "settingsFieldEditor-" + fieldDelegate.index : ""
                    text: root.stringValue(fieldDelegate.fieldValue)
                    placeholderText: fieldDelegate.placeholder
                    echoMode: fieldDelegate.fieldType === 1 ? TextInput.Password : TextInput.Normal
                    enabled: fieldDelegate.key !== "apiKey" || !root.controller.checking
                    onTextEdited: root.controller.setValue(fieldDelegate.index, text)
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: fieldDelegate.key === "apiKey"
                    spacing: 6

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Open Yandex AI Studio, sign in, and click “Create API key” in the top-right corner. Copy the secret key before closing the dialog.")
                        color: palette.text
                        opacity: 0.72
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Button {
                            text: qsTr("Get API key")
                            onClicked: Qt.openUrlExternally("https://aistudio.yandex.ru/platform")
                        }

                        Button {
                            text: root.controller.checking ? qsTr("Checking…") : qsTr("Check key")
                            enabled: !root.controller.checking
                            onClicked: root.controller.checkApiKey()
                        }

                        BusyIndicator {
                            running: root.controller.checking
                            visible: running
                            implicitWidth: 24
                            implicitHeight: 24
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.RichText
                        text: qsTr("Manual setup: <a href=\"https://aistudio.yandex.ru/platform\">aistudio.yandex.ru/platform</a>")
                        color: palette.text
                        opacity: 0.72
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                        onLinkActivated: link => Qt.openUrlExternally(link)
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: root.controller.keyStatus.length > 0
                        text: root.controller.keyStatus
                        color: root.controller.keyStatusError ? palette.text : palette.link
                        font.bold: root.controller.keyStatusError
                        wrapMode: Text.WordWrap
                    }
                }

                ScrollView {
                    Layout.fillWidth: true
                    visible: fieldDelegate.fieldType === 2
                    objectName: visible ? "settingsFieldEditor-" + fieldDelegate.index : ""
                    implicitHeight: visible ? 110 : 0

                    TextArea {
                        text: root.stringValue(fieldDelegate.fieldValue)
                        placeholderText: fieldDelegate.placeholder
                        wrapMode: TextEdit.Wrap
                        onTextChanged: {
                            if (activeFocus)
                                root.controller.setValue(fieldDelegate.index, text)
                        }
                    }
                }

                Switch {
                    visible: fieldDelegate.fieldType === 3
                    objectName: visible ? "settingsFieldEditor-" + fieldDelegate.index : ""
                    checked: Boolean(fieldDelegate.fieldValue)
                    onToggled: root.controller.setValue(fieldDelegate.index, checked)
                }

                SpinBox {
                    Layout.fillWidth: true
                    visible: fieldDelegate.fieldType === 4
                    objectName: visible ? "settingsFieldEditor-" + fieldDelegate.index : ""
                    from: fieldDelegate.minimum
                    to: fieldDelegate.maximum
                    value: Number(fieldDelegate.fieldValue)
                    editable: true
                    onValueModified: root.controller.setValue(fieldDelegate.index, value)
                }

                ComboBox {
                    Layout.fillWidth: true
                    visible: fieldDelegate.fieldType === 5
                    objectName: visible ? "settingsFieldEditor-" + fieldDelegate.index : ""
                    model: fieldDelegate.options
                    currentIndex: Math.max(0, fieldDelegate.options.indexOf(fieldDelegate.fieldValue))
                    onActivated: root.controller.setValue(fieldDelegate.index, currentValue)
                }

                TextArea {
                    Layout.fillWidth: true
                    visible: fieldDelegate.fieldType === 6
                    objectName: visible ? "settingsFieldEditor-" + fieldDelegate.index : ""
                    readOnly: true
                    text: fieldDelegate.key === "usage"
                          ? root.controller.usageSummary
                          : root.stringValue(fieldDelegate.fieldValue)
                    textFormat: TextEdit.AutoText
                    wrapMode: TextEdit.Wrap
                    background: null
                    selectByMouse: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: fieldDelegate.key === "usage"

                    Button {
                        text: qsTr("Reset statistics")
                        onClicked: resetUsageDialog.open()
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Resets only QtNote’s local estimate.")
                        color: palette.text
                        opacity: 0.72
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: fieldDelegate.description.length > 0 || fieldDelegate.restartRequired
                    text: fieldDelegate.description
                          + (fieldDelegate.restartRequired
                             ? (fieldDelegate.description.length > 0 ? "\n" : "")
                               + qsTr("Applied after restarting QtNote.")
                             : "")
                    color: palette.text
                    opacity: 0.72
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
