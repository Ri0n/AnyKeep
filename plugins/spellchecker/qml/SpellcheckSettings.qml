pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var controller

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: root.controller.usingSystemLanguages
                      ? qsTr("Using the system language preferences")
                      : qsTr("Using a custom language selection")
                wrapMode: Text.WordWrap
            }

            Button {
                text: qsTr("Use system languages")
                enabled: !root.controller.usingSystemLanguages
                onClicked: root.controller.useSystemLanguages()
            }
        }

        ListView {
            id: dictionariesView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.controller.dictionariesModel

            delegate: ItemDelegate {
                id: dictionaryDelegate

                required property int index
                required property string displayName
                required property bool selected
                required property bool systemSelected
                required property bool installed
                required property bool removable
                required property bool busy
                required property string actionText
                required property string errorString

                width: dictionariesView.width
                implicitHeight: 48
                rightPadding: actionButton.visible ? actionButton.width + 18 : 10

                contentItem: RowLayout {
                    spacing: 8

                    CheckBox {
                        checked: dictionaryDelegate.selected
                        onToggled: root.controller.setDictionarySelected(dictionaryDelegate.index, checked)
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1

                        Label {
                            Layout.fillWidth: true
                            text: dictionaryDelegate.displayName
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: dictionaryDelegate.errorString.length > 0
                                     || dictionaryDelegate.systemSelected
                            text: dictionaryDelegate.errorString.length > 0
                                  ? dictionaryDelegate.errorString
                                  : qsTr("Selected by the operating system")
                            color: dictionaryDelegate.errorString.length > 0
                                   ? palette.brightText : palette.mid
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }
                }

                Button {
                    id: actionButton
                    anchors.right: parent.right
                    anchors.rightMargin: 4
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !dictionaryDelegate.installed || dictionaryDelegate.removable
                    enabled: !dictionaryDelegate.busy
                    text: dictionaryDelegate.busy ? qsTr("Working…") : dictionaryDelegate.actionText
                    onClicked: root.controller.toggleDictionary(dictionaryDelegate.index)
                }
            }

            Label {
                anchors.centerIn: parent
                visible: dictionariesView.count === 0
                text: qsTr("No dictionaries are available.")
                color: palette.mid
            }
        }
    }
}
