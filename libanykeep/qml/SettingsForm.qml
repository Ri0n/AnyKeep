pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flickable {
    id: root

    required property var controller
    readonly property bool scrollBarAtWindowEdge: true
    property int nonScrollableRightPadding: 12
    readonly property int verticalScrollBarInset:
        contentHeight > height
        ? Math.ceil(Math.max(verticalScrollBar.width, verticalScrollBar.implicitWidth)) : 0
    readonly property int contentRightPadding:
        verticalScrollBarInset > 0 ? 8 : nonScrollableRightPadding
    contentWidth: Math.max(0, width - verticalScrollBarInset - contentRightPadding)
    contentHeight: fieldsColumn.implicitHeight + 24
    clip: true

    function stringValue(value) {
        return value === undefined || value === null ? "" : String(value)
    }

    ScrollBar.vertical: ScrollBar { id: verticalScrollBar }

    ColumnLayout {
        id: fieldsColumn
        width: root.contentWidth
        spacing: 12

        Repeater {
            model: root.controller

            delegate: ColumnLayout {
                id: fieldDelegate

                required property int index
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
                    onTextEdited: root.controller.setValue(fieldDelegate.index, text)
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
                    text: root.stringValue(fieldDelegate.fieldValue)
                    textFormat: TextEdit.AutoText
                    wrapMode: TextEdit.Wrap
                    background: null
                    selectByMouse: true
                }

                Label {
                    Layout.fillWidth: true
                    visible: fieldDelegate.description.length > 0 || fieldDelegate.restartRequired
                    text: fieldDelegate.description
                          + (fieldDelegate.restartRequired
                             ? (fieldDelegate.description.length > 0 ? "\n" : "")
                               + qsTr("Applied after restarting AnyKeep.")
                             : "")
                    color: palette.mid
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
