import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: page

    required property string pluginId
    required property string pluginName
    property var controller: null
    property url contentSource: ""
    signal backRequested()

    function closePage() {
        if (controller && controller.dirty)
            controller.reset()
        backRequested()
    }

    Component.onCompleted: {
        controller = mobileApp.createPluginSettingsController(pluginId, page)
        contentSource = mobileApp.pluginSettingsComponent(pluginId)
        if (controller && contentSource.toString().length > 0)
            settingsLoader.setSource(contentSource, { "controller": controller })
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent

            ToolButton {
                text: qsTr("Back")
                onClicked: page.closePage()
            }
            Label {
                Layout.fillWidth: true
                text: page.pluginName
                font.pixelSize: 20
                font.bold: true
                elide: Text.ElideRight
            }
            ToolButton {
                visible: page.controller !== null
                enabled: page.controller && page.controller.dirty
                text: qsTr("Save")
                onClicked: {
                    if (page.controller.apply())
                        page.backRequested()
                }
            }
        }
    }

    Loader {
        id: settingsLoader
        anchors.fill: parent
        anchors.margins: 12
    }

    Label {
        anchors.centerIn: parent
        width: Math.min(parent.width - 32, 340)
        visible: page.controller === null
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("This plugin does not expose settings on this platform.")
        color: palette.mid
    }

    Label {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        visible: page.controller && page.controller.errorString.length > 0
        text: page.controller ? page.controller.errorString : ""
        color: palette.brightText
        wrapMode: Text.WordWrap
    }
}
