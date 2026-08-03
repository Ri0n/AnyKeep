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
    contentHeight: contentColumn.implicitHeight + 20
    clip: true

    ScrollBar.vertical: ScrollBar { id: verticalScrollBar }

    ColumnLayout {
        id: contentColumn
        width: root.contentWidth
        spacing: 12

        Loader {
            id: genericSettingsLoader
            Layout.fillWidth: true
            Layout.preferredHeight: item ? item.contentHeight : 0
            Component.onCompleted: setSource("qrc:/qml/SettingsForm.qml", {
                "controller": root.controller,
                "nonScrollableRightPadding": 0
            })
        }

        GroupBox {
            title: qsTr("Storage encryption key")
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: root.controller.keyState
                    wrapMode: Text.WordWrap
                }

                TextField {
                    id: recoveryKey
                    Layout.fillWidth: true
                    placeholderText: "private-notes-key-v1:…"
                    echoMode: TextInput.Password
                    text: root.controller.recoveryKey
                    onTextEdited: root.controller.recoveryKey = text
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6

                    Button { text: qsTr("Create key"); onClicked: root.controller.requestCreateKey() }
                    Button { text: qsTr("Import"); onClicked: root.controller.requestImportKey() }
                    Button {
                        text: qsTr("Export")
                        onClicked: {
                            root.controller.requestExportKey()
                            recoveryKey.echoMode = TextInput.Normal
                            recoveryKey.forceActiveFocus()
                            recoveryKey.selectAll()
                        }
                    }
                    Button { text: qsTr("Sync via OMEMO"); onClicked: root.controller.requestOmemoSync() }
                }
            }
        }

        GroupBox {
            title: qsTr("OMEMO devices")
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: root.controller.ownOmemoDevice
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.controller.omemoStatus
                    wrapMode: Text.WordWrap
                    color: palette.mid
                }

                ComboBox {
                    id: devices
                    Layout.fillWidth: true
                    model: root.controller.omemoDevices
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Button { text: qsTr("Refresh devices"); onClicked: root.controller.requestOmemoDevices() }
                    Button {
                        text: qsTr("Trust selected device")
                        enabled: devices.currentIndex >= 0
                        onClicked: root.controller.requestTrustOmemoDevice(devices.currentIndex)
                    }
                    Button {
                        visible: root.controller.repairAvailable
                        text: qsTr("Repair OMEMO device")
                        onClicked: repairDialog.open()
                    }
                }
            }
        }

        GroupBox {
            title: qsTr("PubSub maintenance")
            Layout.fillWidth: true

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Scan for obsolete pre-XML or malformed private-note items. Records using an unknown future version or a different encryption key are reported but never deleted.")
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        text: qsTr("Scan obsolete items")
                        enabled: !root.controller.cleanupBusy
                        onClicked: root.controller.requestScanObsoleteItems()
                    }
                    Button {
                        text: qsTr("Delete %1 obsolete items").arg(root.controller.obsoleteItemCount)
                        visible: root.controller.obsoleteItemCount > 0
                        enabled: !root.controller.cleanupBusy
                        onClicked: cleanupDialog.open()
                    }
                    BusyIndicator {
                        running: root.controller.cleanupBusy
                        visible: running
                    }
                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.controller.cleanupStatus
                    wrapMode: Text.WordWrap
                    color: palette.mid
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Index metadata and note contents are end-to-end encrypted with independent derived keys. The recovery key grants access to the complete XMPP storage.")
            wrapMode: Text.WordWrap
            color: palette.mid
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("TLS is mandatory. Host and port should normally remain automatic.")
            wrapMode: Text.WordWrap
            color: palette.mid
        }
    }

    Dialog {
        id: repairDialog
        title: qsTr("Repair OMEMO device")
        modal: true
        standardButtons: Dialog.Reset | Dialog.Cancel
        anchors.centerIn: Overlay.overlay

        Label {
            width: Math.min(420, root.width - 40)
            text: qsTr("The published bundle for this OMEMO device is incomplete. Repairing it creates a new OMEMO device ID and identity key, resets OMEMO sessions and trust decisions, and requires the new device to be trusted again on your other clients. The private-notes storage key is not changed.")
            wrapMode: Text.WordWrap
        }

        onReset: root.controller.requestRepairOmemoDevice()
    }

    Dialog {
        id: cleanupDialog
        title: qsTr("Delete obsolete PubSub items")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: Overlay.overlay

        Label {
            width: Math.min(420, root.width - 40)
            text: qsTr("Delete %1 obsolete or malformed encrypted items from the XMPP PubSub nodes? The records are fetched and checked again immediately before deletion. Unsupported future formats and records protected by another key are left untouched.").arg(root.controller.obsoleteItemCount)
            wrapMode: Text.WordWrap
        }

        onAccepted: root.controller.requestDeleteObsoleteItems()
    }

    Component.onCompleted: root.controller.requestOmemoDevices()
}
