import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    signal backRequested()
    signal openGeneral()
    signal openDrafts()
    signal openStorages()
    signal openPlugins()

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            ToolButton {
                text: qsTr("‹")
                font.pixelSize: 27
                Accessible.name: qsTr("Back")
                onClicked: root.backRequested()
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Settings")
                font.pixelSize: 20
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }
            Item { Layout.preferredWidth: 40 }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 4

        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("General")
            onClicked: root.openGeneral()
        }
        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("Drafts (%1)").arg(mobileApp.recoverableDrafts.length)
            onClicked: root.openDrafts()
        }
        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("Storages")
            onClicked: root.openStorages()
        }
        ItemDelegate {
            Layout.fillWidth: true
            text: qsTr("Plugins")
            onClicked: root.openPlugins()
        }
        Item { Layout.fillHeight: true }
    }
}
