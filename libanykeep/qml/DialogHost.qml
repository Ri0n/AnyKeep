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

        contentItem: ScrollView {
            implicitWidth: Math.min(520, Math.max(240, root.width - 48))
            implicitHeight: Math.min(360, Math.max(80, messageText.contentHeight))

            TextArea {
                id: messageText
                readOnly: true
                background: null
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.RichText
                text: root.dialogService ? root.dialogService.message : ""
                onLinkActivated: link => Qt.openUrlExternally(link)
            }
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
