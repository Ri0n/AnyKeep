import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: customDictionaryDialog
    required property var editorView

    function openEditor() {
        if (!editorView.platformBackend)
            return
        customDictionaryText.text = editorView.platformBackend.customSpellingDictionary().join("\n")
        open()
        customDictionaryText.forceActiveFocus()
    }
    parent: Overlay.overlay
    title: qsTr("Custom Dictionary")
    modal: true
    standardButtons: Dialog.Save | Dialog.Cancel
    width: Math.min(480, Math.max(280, parent ? parent.width - 32 : 420))
    height: Math.min(520, Math.max(260, parent ? parent.height - 48 : 420))
    onAccepted: {
        if (editorView.platformBackend)
            editorView.platformBackend.setCustomSpellingDictionary(customDictionaryText.text.split(/\r?\n/))
    }

    ColumnLayout {
        anchors.fill: parent

        Label {
            Layout.fillWidth: true
            text: qsTr("One word per line")
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TextArea {
                id: customDictionaryText
                objectName: "customDictionaryText"
                wrapMode: TextEdit.NoWrap
                selectByMouse: true
            }
        }
    }
}
