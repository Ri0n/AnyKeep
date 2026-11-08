import QtQuick
import QtQuick.Controls

TextField {
    id: titleField
    required property var mediaBlock
    property bool editable: true
    property bool editing: false
    property bool reportedFocus: false
    property bool historyTransaction: false
    objectName: "mediaInlineTitle-" + mediaBlock.block.index
    text: mediaBlock.block.alt
    placeholderText: mediaBlock.video ? qsTr("Video title") : qsTr("Audio title")
    readOnly: !editing
    selectByMouse: editing
    color: mediaBlock.video ? "white" : mediaBlock.editorView.documentTextColor
    placeholderTextColor: mediaBlock.video ? "#c0ffffff" : palette.placeholderText
    font.bold: mediaBlock.selected
    padding: 3
    background: Rectangle {
        radius: 3
        color: titleField.editing ? (mediaBlock.video ? "#60000000" : titleField.palette.base) : "transparent"
        border.width: titleField.editing ? 1 : 0
        border.color: titleField.palette.highlight
    }

    function reportFocus(value) {
        if (reportedFocus === value)
            return
        reportedFocus = value
        mediaBlock.titleEditing = value
        mediaBlock.editorView.mediaTitleEditorFocused = value
    }
    function startEditing() {
        mediaBlock.selectAndFocus()
        editing = true
        Qt.callLater(function() { titleField.forceActiveFocus(Qt.MouseFocusReason) })
    }
    onTextEdited: mediaBlock.setTitle(text)
    onEditingChanged: {
        if (editing && !historyTransaction) {
            mediaBlock.editorView.beginEditTransaction("rename-media")
            historyTransaction = true
        } else if (!editing && historyTransaction) {
            Qt.inputMethod.commit()
            historyTransaction = false
            mediaBlock.editorView.endEditTransaction()
        }
        reportFocus(activeFocus && editing)
        if (!editing)
            focus = false
    }
    onActiveFocusChanged: {
        reportFocus(activeFocus && editing)
        if (!activeFocus)
            editing = false
    }
    onAccepted: {
        editing = false
        mediaBlock.selectAndFocus()
    }
    Keys.onShortcutOverride: function(event) {
        if (event.key === Qt.Key_Escape && editing)
            event.accepted = true
    }
    Keys.onEscapePressed: {
        editing = false
        mediaBlock.selectAndFocus()
    }
    TapHandler {
        enabled: titleField.editable && !titleField.editing
        onTapped: titleField.startEditing()
    }
    Component.onDestruction: {
        editing = false
        reportFocus(false)
    }
    Connections {
        target: mediaBlock
        ignoreUnknownSignals: true
        function onFinishTitleEditing() { titleField.editing = false }
    }
    Accessible.name: placeholderText
}
