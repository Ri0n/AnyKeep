import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    property var settingsController: qtnoteSettingsController
    property url settingsContentSource: qtnoteSettingsComponent
    property string settingsTitle: qtnoteSettingsTitle
    signal settingsApplied()

    width: 560
    height: 620
    minimumWidth: 360
    minimumHeight: 320
    title: settingsTitle

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        spacing: 10

        Loader {
            id: contentLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.rightMargin: item && item.scrollBarAtWindowEdge === true ? 0 : 12
            // Scrollable settings pages reach the window edge.  The page
            // itself keeps content clear of its scrollbar.

            function loadSettings() {
                if (!window.settingsController || window.settingsContentSource.toString().length === 0) {
                    source = ""
                    return
                }
                setSource(window.settingsContentSource, { "controller": window.settingsController })
            }

            Component.onCompleted: loadSettings()
            Connections {
                target: window
                function onSettingsControllerChanged() { contentLoader.loadSettings() }
                function onSettingsContentSourceChanged() { contentLoader.loadSettings() }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.rightMargin: 12
            visible: window.settingsController
                     && window.settingsController.errorString.length > 0
            text: window.settingsController ? window.settingsController.errorString : ""
            color: palette.brightText
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.rightMargin: 12

            Button {
                text: qsTr("Reset")
                enabled: window.settingsController && window.settingsController.dirty
                onClicked: window.settingsController.reset()
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Cancel")
                onClicked: {
                    if (window.settingsController)
                        window.settingsController.reset()
                    window.close()
                }
            }

            Button {
                text: qsTr("Save")
                highlighted: true
                enabled: window.settingsController
                onClicked: {
                    if (window.settingsController && window.settingsController.apply()) {
                        window.settingsApplied()
                        window.close()
                    }
                }
            }
        }
    }
}
