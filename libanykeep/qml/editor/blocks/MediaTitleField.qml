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
    // Audio has no placeholder: show the real filename until an explicit,
    // editable media title has been supplied. Keep a native single-line
    // TextField so the caret can scroll through text beyond the viewport.
    // Media references can arrive after the note: refresh the filename when
    // a remote manifest is reconciled, without changing the document title.
    property int mediaManifestRevision: 0
    readonly property string audioFileName: {
        const revision = mediaManifestRevision
        return mediaBlock.audio && mediaBlock.editorView.editorBackend
               ? mediaBlock.editorView.editorBackend.mediaFileName(mediaBlock.block.url) : ""
    }
    // A TextField must own its text while editing. A live binding to the
    // model replaces the first keystroke after Select All (and can break IME
    // composition). Reattach the model/filename only after the edit transaction
    // has finished, never while it is active.
    Binding {
        target: titleField
        property: "text"
        value: mediaBlock.audio && !mediaBlock.block.alt
               ? titleField.audioFileName : mediaBlock.block.alt
        when: !titleField.editing && !titleField.historyTransaction
        restoreMode: Binding.RestoreNone
    }
    placeholderText: mediaBlock.video ? qsTr("Video title") : ""
    clip: true
    autoScroll: true
    verticalAlignment: TextInput.AlignVCenter
    readOnly: !editing
    selectByMouse: editing
    color: mediaBlock.video ? "white" : mediaBlock.editorView.documentTextColor
    placeholderTextColor: mediaBlock.video ? "#c0ffffff" : palette.placeholderText
    font.bold: mediaBlock.selected
    // Some Qt Quick Controls styles set individual paddings (notably
    // Material's TextField) even when Control.padding is overridden. Specify
    // each side explicitly so the audio title's visible glyphs align with the
    // time label and the surrounding card's symmetric content insets.
    // Preserve the pre-existing video title geometry.
    padding: mediaBlock.audio ? 1 : 3
    leftPadding: mediaBlock.audio ? 1 : 3
    rightPadding: mediaBlock.audio ? 1 : 3
    topPadding: mediaBlock.audio ? 1 : 3
    bottomPadding: mediaBlock.audio ? 1 : 3
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
        target: mediaBlock.editorView.editorBackend
        ignoreUnknownSignals: true
        function onMediaChanged() { ++titleField.mediaManifestRevision }
    }
    Connections {
        target: mediaBlock
        ignoreUnknownSignals: true
        function onFinishTitleEditing() { titleField.editing = false }
    }
    Accessible.name: placeholderText
}
