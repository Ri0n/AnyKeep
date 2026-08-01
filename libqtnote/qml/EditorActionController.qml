import QtQuick

QtObject {
    id: root

    required property var editorBackend
    required property var blockEditor
    property var platformBackend: null

    readonly property int bulletListType: 1
    readonly property int taskListType: 2
    readonly property int numberedListType: 5

    function runMarkdownCommand(kind, command) {
        if (!root.editorBackend || !root.blockEditor)
            return false
        root.blockEditor.flushPendingEditorChanges()
        const beforeView = root.blockEditor.captureEditorState()
        root.editorBackend.beginHistoryTransaction(kind, beforeView)
        try {
            if (!root.editorBackend.markdown)
                root.editorBackend.markdown = true
            return command()
        } finally {
            root.editorBackend.endHistoryTransaction(root.blockEditor.captureEditorState())
        }
    }

    function insertList(type) {
        const wasMarkdown = root.editorBackend && root.editorBackend.markdown
        const row = root.blockEditor ? root.blockEditor.insertionBlockIndex() : 0
        return runMarkdownCommand("insert-or-convert-list", function() {
            if (wasMarkdown)
                return root.blockEditor.insertListBlock(type)
            root.blockEditor.blockModel.insertList(row, type)
            root.blockEditor.focusBlock(row)
            return true
        })
    }

    function insertCodeBlock() {
        return runMarkdownCommand("insert-code-block", function() {
            return root.blockEditor.insertCodeBlock("")
        })
    }

    function insertTable() {
        const row = root.blockEditor ? root.blockEditor.insertionBlockIndex() : 0
        return runMarkdownCommand("insert-table", function() {
            root.blockEditor.blockModel.insertTable(row)
            root.blockEditor.focusBlock(row)
            return true
        })
    }

    function insertBlockQuote() {
        return runMarkdownCommand("insert-or-convert-blockquote", function() {
            return root.blockEditor.insertBlockQuoteBlock()
        })
    }

    function prepareMediaInsertion(callback) {
        if (!root.platformBackend || !root.editorBackend || !root.blockEditor)
            return false
        root.blockEditor.flushPendingEditorChanges()
        const row = root.blockEditor.insertionBlockIndex()
        Qt.callLater(function() { callback(row) })
        return true
    }

    function insertImage() {
        return prepareMediaInsertion(function(row) { root.platformBackend.insertImage(row) })
    }

    function insertPhoto() {
        if (!root.platformBackend || typeof root.platformBackend.insertPhoto !== "function")
            return false
        return prepareMediaInsertion(function(row) { root.platformBackend.insertPhoto(row) })
    }

    function insertAttachment() {
        if (!root.platformBackend || typeof root.platformBackend.insertAttachment !== "function")
            return false
        return prepareMediaInsertion(function(row) { root.platformBackend.insertAttachment(row) })
    }

    function copyDocument() {
        if (!root.editorBackend || !root.blockEditor)
            return false
        root.blockEditor.flushPendingEditorChanges()
        root.editorBackend.copyDocumentToClipboard()
        return true
    }

    function setMarkdownMode(markdown) {
        if (!root.editorBackend || !root.blockEditor || root.editorBackend.markdown === markdown)
            return
        root.blockEditor.flushPendingEditorChanges()
        root.editorBackend.markdown = markdown
    }

    function toggleMarkdownMode() {
        if (root.editorBackend)
            setMarkdownMode(!root.editorBackend.markdown)
    }
}
