pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property Item hostItem
    required property var controller

    parent: hostItem
    anchors.fill: parent
    z: 100000

    function pageTitle() {
        switch (controller.currentPage) {
        case 0:
            return controller.localKeyMissing
                    ? qsTr("The local XMPP storage key is missing")
                    : qsTr("AnyKeep found incompatible storage keys")
        case 1: return qsTr("Verify your AnyKeep devices")
        case 2: return qsTr("Choose the key to keep")
        case 3: return qsTr("Review and repair")
        case 4: return qsTr("Recovery result")
        default: return ""
        }
    }

    function pageSubtitle() {
        switch (controller.currentPage) {
        case 0:
            return controller.localKeyMissing
                    ? qsTr("AnyKeep can securely obtain the key from another online device on this account.")
                    : qsTr("Notes or another AnyKeep device use a different encryption key.")
        case 1:
            return qsTr("Select devices you recognize. Their fingerprints are used only to establish encrypted OMEMO sessions.")
        case 2:
            return qsTr("AnyKeep grouped existing notes and online devices by their storage-key fingerprint.")
        case 3:
            return qsTr("AnyKeep is ready to republish accessible notes with the selected key.")
        case 4:
            return ""
        default:
            return ""
        }
    }

    Dialog {
        id: dialog

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(280, Math.min(760, root.width - 16))
        height: Math.max(300, Math.min(720, root.height - 16))
        modal: true
        closePolicy: Popup.NoAutoClose
        title: qsTr("Repair XMPP note synchronization")

        contentItem: ColumnLayout {
            spacing: 10

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: root.pageTitle()
                    font.pixelSize: 18
                    font.bold: true
                    wrapMode: Text.WordWrap
                }

                Label {
                    text: qsTr("%1 / %2").arg(root.controller.currentPage + 1).arg(root.controller.pageCount)
                    color: palette.mid
                }
            }

            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.pageSubtitle()
                wrapMode: Text.WordWrap
                color: palette.mid
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: palette.mid
                opacity: 0.35
            }

            StackLayout {
                id: pages

                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: root.controller.currentPage

                ScrollView {
                    clip: true

                    ColumnLayout {
                        width: pages.width
                        spacing: 14

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("This recovery flow locates your other online AnyKeep devices, establishes trusted OMEMO sessions, collects the storage keys they hold, and safely moves every accessible note to one key you choose.")
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("No note or local key is changed until the final recovery step completes. You can cancel now and run the recovery again later.")
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                ColumnLayout {
                    spacing: 8

                    ListView {
                        id: devicesView

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 180
                        clip: true
                        spacing: 4
                        model: root.controller.devicesModel
                        boundsBehavior: Flickable.StopAtBounds
                        reuseItems: true

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }

                        delegate: Frame {
                            id: deviceDelegate

                            required property int index
                            required property string label
                            required property string fingerprint
                            required property string trustText
                            required property bool selected
                            required property bool selectable
                            required property bool trusted

                            width: ListView.view.width
                            padding: 6

                            contentItem: RowLayout {
                                id: deviceRow
                                spacing: 8

                                CheckBox {
                                    Layout.alignment: Qt.AlignTop
                                    checked: deviceDelegate.selected
                                    enabled: deviceDelegate.selectable && !root.controller.busy
                                    onClicked: root.controller.setDeviceSelected(deviceDelegate.index, checked)
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1

                                    Label {
                                        Layout.fillWidth: true
                                        text: deviceDelegate.label
                                        font.bold: true
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: deviceDelegate.fingerprint
                                        wrapMode: Text.WordWrap
                                        color: palette.mid
                                        font.family: "monospace"
                                        font.pixelSize: 11
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: deviceDelegate.trustText
                                        color: deviceDelegate.trusted ? palette.highlight : palette.mid
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                    }
                                }
                            }
                        }

                        Label {
                            anchors.centerIn: parent
                            visible: devicesView.count === 0
                            width: Math.min(parent.width - 24, 420)
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("No OMEMO devices are currently available. Start AnyKeep on another device and retry.")
                            wrapMode: Text.WordWrap
                            color: palette.mid
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.controller.deviceStatus
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Compare fingerprints on the other device. Leave unfamiliar devices unchecked.")
                        wrapMode: Text.WordWrap
                        color: palette.mid
                        font.pixelSize: 12
                    }
                }

                ColumnLayout {
                    spacing: 8

                    ListView {
                        id: keysView

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 180
                        clip: true
                        spacing: 4
                        model: root.controller.keysModel
                        boundsBehavior: Flickable.StopAtBounds
                        reuseItems: true

                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }

                        delegate: Frame {
                            id: keyDelegate

                            required property int index
                            required property string fingerprint
                            required property string source
                            required property int noteCount
                            required property string status
                            required property bool available

                            width: ListView.view.width
                            opacity: keyDelegate.available ? 1.0 : 0.55
                            padding: 6

                            contentItem: RowLayout {
                                spacing: 8

                                RadioButton {
                                    checked: root.controller.selectedKeyIndex === keyDelegate.index
                                    enabled: keyDelegate.available && !root.controller.busy
                                    onClicked: root.controller.selectKey(keyDelegate.index)
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: keyDelegate.fingerprint
                                        font.bold: true
                                        font.family: "monospace"
                                        font.pixelSize: 12
                                        elide: Text.ElideRight
                                        maximumLineCount: 1
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Available from: %1").arg(keyDelegate.source)
                                        wrapMode: Text.WordWrap
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Notes: %1 — %2").arg(keyDelegate.noteCount).arg(keyDelegate.status)
                                        color: palette.mid
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.controller.keyStatus
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Usually you should keep the key that owns the most notes. A key marked unavailable cannot be selected; bring one of its devices online or import its recovery key first.")
                        wrapMode: Text.WordWrap
                        color: palette.mid
                    }
                }

                ScrollView {
                    clip: true

                    Label {
                        width: pages.width
                        text: root.controller.summary
                        wrapMode: Text.WordWrap
                    }
                }

                ScrollView {
                    clip: true

                    Label {
                        width: pages.width
                        text: root.controller.resultText
                        wrapMode: Text.WordWrap
                    }
                }
            }

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: root.controller.busy
                visible: running
                implicitWidth: 28
                implicitHeight: 28
            }

            GridLayout {
                Layout.fillWidth: true
                columns: dialog.width < 420 ? 2 : 3
                columnSpacing: 8
                rowSpacing: 8

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Cancel")
                    enabled: root.controller.canCancel
                    onClicked: root.controller.cancel()
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Back")
                    enabled: root.controller.canGoBack
                    onClicked: root.controller.back()
                }

                Button {
                    Layout.fillWidth: true
                    Layout.columnSpan: dialog.width < 420 ? 2 : 1
                    text: root.controller.nextText
                    highlighted: true
                    enabled: root.controller.canGoNext
                    onClicked: root.controller.next()
                }
            }
        }

        onClosed: root.destroy()
    }

    Connections {
        target: root.controller

        function onFinished(accepted) {
            dialog.close()
        }
    }

    Component.onCompleted: dialog.open()
}
