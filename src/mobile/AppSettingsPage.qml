import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    Flickable {
        anchors.fill: parent
        contentHeight: form.implicitHeight + 24
        clip: true

        ColumnLayout {
            id: form
            x: 16
            y: 12
            width: parent.width - 32
            spacing: 16

            Switch {
                Layout.fillWidth: true
                text: qsTr("Ask before deleting a note")
                checked: mobileApp.askBeforeDelete
                onToggled: mobileApp.askBeforeDelete = checked
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: mobileApp.androidSpeechAvailable
                spacing: 4

                Switch {
                    Layout.fillWidth: true
                    text: qsTr("Use Android speech recognition")
                    checked: mobileApp.androidSpeechEnabled
                    onToggled: mobileApp.androidSpeechEnabled = checked
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.7
                    text: qsTr("When enabled, QtNote shows a microphone button and asks Android for microphone permission. Recognition is provided by the speech service installed on the device.")
                }
            }

            Label { text: qsTr("Notes loaded per page: %1").arg(notesSlider.value) }
            Slider {
                id: notesSlider
                Layout.fillWidth: true
                from: 10
                to: 200
                stepSize: 10
                value: mobileApp.notesPerPage
                onMoved: mobileApp.notesPerPage = value
            }

            Label { text: qsTr("Editor font size: %1").arg(fontSlider.value.toFixed(0)) }
            Slider {
                id: fontSlider
                Layout.fillWidth: true
                from: 10
                to: 36
                stepSize: 1
                value: mobileApp.editorFontSize
                onMoved: mobileApp.editorFontSize = value
            }
        }
    }
}
