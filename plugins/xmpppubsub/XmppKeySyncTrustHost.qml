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
    z: 100001

    Dialog {
        id: dialog

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(300, Math.min(520, root.width - 24))
        modal: true
        closePolicy: Popup.NoAutoClose
        title: qsTr("Trust an own QtNote device?")

        contentItem: ColumnLayout {
            spacing: 12

            Label {
                Layout.fillWidth: true
                text: qsTr("Another device on your XMPP account wants to synchronize the QtNote storage key.")
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("OMEMO fingerprint:")
                font.bold: true
            }

            Label {
                Layout.fillWidth: true
                text: root.controller.fingerprint
                font.family: "monospace"
                wrapMode: Text.WrapAnywhere
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Only approve this request if you recognize the device or can compare this fingerprint on both devices.")
                wrapMode: Text.WordWrap
                color: palette.mid
            }

            GridLayout {
                Layout.fillWidth: true
                columns: dialog.width < 420 ? 1 : 2
                columnSpacing: 8
                rowSpacing: 8

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Reject")
                    onClicked: root.controller.reject()
                }

                Button {
                    Layout.fillWidth: true
                    text: qsTr("Trust and send key")
                    onClicked: root.controller.accept()
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
