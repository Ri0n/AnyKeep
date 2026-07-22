import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property var dialogService

    anchors.fill: parent
    z: 10000

    Component.onCompleted: {
        if (root.dialogService && root.dialogService.active)
            dialog.open()
    }

    Connections {
        target: root.dialogService
        function onActiveChanged() {
            if (root.dialogService.active)
                dialog.open()
            else
                dialog.close()
        }
    }

    Dialog {
        id: dialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        closePolicy: Popup.NoAutoClose
        title: root.dialogService ? root.dialogService.title : ""

        contentItem: Label {
            width: Math.min(420, root.width - 48)
            wrapMode: Text.WordWrap
            text: root.dialogService ? root.dialogService.message : ""
        }

        footer: RowLayout {
            spacing: 8

            Item { Layout.fillWidth: true }

            Button {
                visible: root.dialogService && root.dialogService.rejectText.length > 0
                text: root.dialogService ? root.dialogService.rejectText : ""
                onClicked: root.dialogService.reject()
            }

            Button {
                text: root.dialogService ? root.dialogService.acceptText : ""
                highlighted: root.dialogService && root.dialogService.destructive
                onClicked: root.dialogService.accept()
            }
        }
    }
}
