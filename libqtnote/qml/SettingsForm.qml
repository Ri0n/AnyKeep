pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flickable {
    id: root

    required property var controller
    contentWidth: width
    contentHeight: fieldsColumn.implicitHeight + 24
    clip: true

    function stringValue(value) {
        return value === undefined || value === null ? "" : String(value)
    }

    ScrollBar.vertical: ScrollBar { }

    ColumnLayout {
        id: fieldsColumn
        width: root.width
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

                Loader {
                    Layout.fillWidth: true
                    sourceComponent: fieldDelegate.fieldType === 3 ? booleanEditor
                                   : fieldDelegate.fieldType === 4 ? integerEditor
                                   : fieldDelegate.fieldType === 5 ? choiceEditor
                                   : fieldDelegate.fieldType === 6 ? readOnlyEditor
                                   : fieldDelegate.fieldType === 2 ? multilineEditor
                                   : textEditor
                }

                Label {
                    Layout.fillWidth: true
                    visible: fieldDelegate.description.length > 0 || fieldDelegate.restartRequired
                    text: fieldDelegate.description
                          + (fieldDelegate.restartRequired
                             ? (fieldDelegate.description.length > 0 ? "\n" : "")
                               + qsTr("Applied after restarting QtNote.")
                             : "")
                    color: palette.mid
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }

                Component {
                    id: textEditor
                    TextField {
                        text: root.stringValue(fieldDelegate.fieldValue)
                        placeholderText: fieldDelegate.placeholder
                        echoMode: fieldDelegate.fieldType === 1 ? TextInput.Password : TextInput.Normal
                        onTextEdited: root.controller.setValue(fieldDelegate.index, text)
                    }
                }

                Component {
                    id: multilineEditor
                    ScrollView {
                        implicitHeight: 110
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
                }

                Component {
                    id: booleanEditor
                    Switch {
                        checked: Boolean(fieldDelegate.fieldValue)
                        onToggled: root.controller.setValue(fieldDelegate.index, checked)
                    }
                }

                Component {
                    id: integerEditor
                    SpinBox {
                        from: fieldDelegate.minimum
                        to: fieldDelegate.maximum
                        value: Number(fieldDelegate.fieldValue)
                        editable: true
                        onValueModified: root.controller.setValue(fieldDelegate.index, value)
                    }
                }

                Component {
                    id: choiceEditor
                    ComboBox {
                        model: fieldDelegate.options
                        currentIndex: Math.max(0, fieldDelegate.options.indexOf(fieldDelegate.fieldValue))
                        onActivated: root.controller.setValue(fieldDelegate.index, currentValue)
                    }
                }

                Component {
                    id: readOnlyEditor
                    TextArea {
                        readOnly: true
                        text: root.stringValue(fieldDelegate.fieldValue)
                        wrapMode: TextEdit.Wrap
                        background: null
                        selectByMouse: true
                    }
                }
            }
        }
    }
}
