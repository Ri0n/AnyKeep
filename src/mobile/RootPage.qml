import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    signal openSettings()

    readonly property int headerButtonSize: 44

    header: ToolBar {
        background: Rectangle { color: palette.window }
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
                objectName: "androidSettingsButton"
                Layout.preferredWidth: root.headerButtonSize
                Layout.preferredHeight: root.headerButtonSize
                padding: 0
                display: AbstractButton.IconOnly
                contentItem: ThemedIcon {
                    themeName: "__bundled__"
                    fallbackName: "preferences-system-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(settingsButton.palette.buttonText)
                    pixelSize: 22
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
