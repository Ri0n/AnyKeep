import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    signal openSettings()

    readonly property int headerButtonSize: 29

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 0

            Item { Layout.preferredWidth: root.headerButtonSize }

            Label {
                Layout.fillWidth: true
                text: qsTr("Notes")
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 20
                font.bold: true
            }

            ToolButton {
                id: settingsButton
                Layout.preferredWidth: root.headerButtonSize
                Layout.preferredHeight: root.headerButtonSize
                padding: 0
                display: AbstractButton.IconOnly
                contentItem: Image {
                    width: 14
                    height: 14
                    source: "image://qtnoteicons/preferences-system-symbolic/preferences-system-symbolic.svg/light"
                    sourceSize.width: 14
                    sourceSize.height: 14
                    fillMode: Image.PreserveAspectFit
                }
                Accessible.name: qsTr("Settings")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: root.openSettings()
            }
        }
    }

    NotesPage {
        anchors.fill: parent
    }
}
