import QtQuick
import QtQuick.Controls

Menu {
    id: menu
    required property var controller
    required property var editorBackend
    required property var platformBackend
    readonly property var editor: controller.contextEditor

    Instantiator {
        model: menu.editor ? menu.editor.contextSuggestions : []
        delegate: MenuItem {
            required property string modelData
            text: modelData
            onTriggered: if (menu.editor) menu.editor.replaceContextWord(modelData)
        }
        onObjectAdded: function(index, object) { menu.insertItem(index, object) }
        onObjectRemoved: function(index, object) { menu.removeItem(object) }
    }
    MenuSeparator { visible: menu.editor && menu.editor.contextWord.length > 0; height: visible ? implicitHeight : 0 }
    MenuItem { visible: menu.editor && menu.editor.tableCell; height: visible ? implicitHeight : 0; text: qsTr("Insert row above"); onTriggered: menu.editor.insertRowAbove() }
    MenuItem { visible: menu.editor && menu.editor.tableCell; height: visible ? implicitHeight : 0; text: qsTr("Insert row below"); onTriggered: menu.editor.insertRowBelow() }
    MenuItem {
        visible: menu.editor && menu.editor.tableCell
        height: visible ? implicitHeight : 0
        text: qsTr("Delete row")
        enabled: menu.editor && menu.editor.canRemoveTableRow
        onTriggered: menu.editor.removeRow()
    }
    MenuSeparator { visible: menu.editor && menu.editor.tableCell; height: visible ? implicitHeight : 0 }
    MenuItem { visible: menu.editor && menu.editor.tableCell; height: visible ? implicitHeight : 0; text: qsTr("Insert column left"); onTriggered: menu.editor.insertColumnLeft() }
    MenuItem { visible: menu.editor && menu.editor.tableCell; height: visible ? implicitHeight : 0; text: qsTr("Insert column right"); onTriggered: menu.editor.insertColumnRight() }
    MenuItem {
        visible: menu.editor && menu.editor.tableCell
        height: visible ? implicitHeight : 0
        text: qsTr("Delete column")
        enabled: menu.editor && menu.editor.canRemoveTableColumn
        onTriggered: menu.editor.removeColumn()
    }
    MenuItem {
        visible: menu.editor && menu.editor.tableCell
        height: visible ? implicitHeight : 0
        text: qsTr("Delete table")
        onTriggered: menu.controller.removeTableBlock(menu.editor.blockIndex, true)
    }
    MenuSeparator { visible: menu.editor && menu.editor.tableCell; height: visible ? implicitHeight : 0 }
    MenuItem {
        visible: menu.platformBackend && menu.editor && menu.editor.contextWord.length > 0
        height: visible ? implicitHeight : 0
        text: qsTr("Add to dictionary")
        onTriggered: {
            menu.platformBackend.addToSpellingDictionary(menu.editor.contextWord)
            menu.editor.refreshSpelling()
        }
    }
    MenuSeparator { visible: menu.platformBackend && menu.editor && menu.editor.contextWord.length > 0; height: visible ? implicitHeight : 0 }
    MenuItem {
        text: qsTr("Spell Check")
        visible: menu.platformBackend !== null
        height: visible ? implicitHeight : 0
        checkable: true
        checked: menu.platformBackend ? menu.platformBackend.spellCheckEnabled : false
        onToggled: if (menu.platformBackend) menu.platformBackend.spellCheckEnabled = checked
    }
    MenuItem {
        text: qsTr("Edit Custom Dictionary…")
        visible: menu.platformBackend !== null
        height: visible ? implicitHeight : 0
        onTriggered: menu.controller.openCustomDictionaryEditor()
    }
    MenuSeparator { visible: menu.platformBackend !== null; height: visible ? implicitHeight : 0 }
    MenuItem {
        action: Action {
            text: menu.editorBackend && menu.editorBackend.undoText.length > 0
                ? qsTr("Undo %1").arg(menu.editorBackend.undoText) : qsTr("Undo")
            shortcut: StandardKey.Undo
            enabled: menu.editorBackend ? menu.editorBackend.canUndo : false
            onTriggered: if (menu.editorBackend) menu.editorBackend.undo()
        }
    }
    MenuItem {
        action: Action {
            text: menu.editorBackend && menu.editorBackend.redoText.length > 0
                ? qsTr("Redo %1").arg(menu.editorBackend.redoText) : qsTr("Redo")
            shortcut: StandardKey.Redo
            enabled: menu.editorBackend ? menu.editorBackend.canRedo : false
            onTriggered: if (menu.editorBackend) menu.editorBackend.redo()
        }
    }
    MenuSeparator {}
    MenuItem {
        objectName: "copyContextLinkMenuItem"
        visible: menu.editor && menu.editor.contextLink.length > 0
        height: visible ? implicitHeight : 0
        text: qsTr("Copy Link")
        onTriggered: if (menu.editorBackend && menu.editor) menu.editorBackend.copyToClipboard(menu.editor.contextLink)
    }
    MenuSeparator { visible: menu.editor && menu.editor.contextLink.length > 0; height: visible ? implicitHeight : 0 }
    MenuItem { action: Action { text: qsTr("Cut"); shortcut: StandardKey.Cut; enabled: menu.controller.documentSelectionAvailable; onTriggered: menu.controller.cutDocumentSelection() } }
    MenuItem { action: Action { text: qsTr("Copy"); shortcut: StandardKey.Copy; enabled: menu.controller.documentSelectionAvailable; onTriggered: menu.controller.copyDocumentSelection() } }
    MenuItem {
        objectName: "copyMarkdownMenuItem"
        readonly property bool formatEnabled: menu.editorBackend ? menu.editorBackend.markdown : false
        visible: formatEnabled
        height: visible ? implicitHeight : 0
        text: qsTr("Copy Markdown")
        enabled: menu.controller.documentSelectionAvailable
        onTriggered: menu.controller.copyDocumentSelectionAsMarkdown()
    }
    MenuItem {
        action: Action {
            text: qsTr("Paste")
            shortcut: StandardKey.Paste
            enabled: menu.editor ? menu.editor.canPaste : false
            onTriggered: {
                if (!menu.editor)
                    return
                // Calling TextArea.paste() directly bypasses BlockTextArea's
                // key handler and lets QTextDocument create an opaque rich
                // text table.  Give the block model first chance to consume
                // every structural clipboard format; retain Qt's normal text
                // paste only as the fallback.
                menu.controller.activeEditor = menu.editor
                menu.controller.pasteClipboard()
            }
        }
    }
    MenuSeparator {}
    MenuItem { action: Action { text: qsTr("Select All"); shortcut: StandardKey.SelectAll; onTriggered: menu.controller.selectAllDocument() } }
}
