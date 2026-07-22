pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: page

    signal openSettings(string pluginId, string pluginName)

    ListView {
        id: pluginsView
        anchors.fill: parent
        anchors.margins: 12
        clip: true
        model: mobileApp.pluginsModel

        delegate: ItemDelegate {
            id: pluginDelegate

            required property int index
            required property string pluginId
            required property string name
            required property string description
            required property string versionText
            required property int loadPolicy
            required property bool loaded
            required property bool configurable
            required property string iconSource

            width: pluginsView.width
            rightPadding: enabledSwitch.width + 24
            onClicked: {
                if (configurable)
                    page.openSettings(pluginId, name)
            }

            contentItem: RowLayout {
                spacing: 10

                Item {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignVCenter

                    Image {
                        id: pluginIcon
                        anchors.fill: parent
                        source: pluginDelegate.iconSource
                        sourceSize.width: 28
                        sourceSize.height: 28
                        fillMode: Image.PreserveAspectFit
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: pluginIcon.status !== Image.Ready
                        text: "⚙"
                        font.pixelSize: 20
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: pluginDelegate.name
                        font.bold: pluginDelegate.loaded
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: pluginDelegate.versionText.length > 0
                              ? qsTr("Version %1").arg(pluginDelegate.versionText)
                              : pluginDelegate.description
                        visible: text.length > 0
                        elide: Text.ElideRight
                        color: palette.mid
                        font.pixelSize: 13
                    }
                }
            }

            Switch {
                id: enabledSwitch
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                checked: pluginDelegate.loadPolicy !== 2
                onToggled: mobileApp.setPluginEnabled(pluginDelegate.index, checked)
            }
        }

        Label {
            anchors.centerIn: parent
            width: Math.min(parent.width - 32, 340)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: pluginsView.count === 0
            text: qsTr("No Android-compatible bundled plugins are registered.")
            color: palette.mid
        }
    }
}
