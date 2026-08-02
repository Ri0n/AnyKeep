import QtQuick
import "../components" as Editor

Editor.NoteBlockTextArea {
    id: textCell
    required property var block
    titleDocument: block.index === 0
    blockIndex: block.index
    width: block.width
    discardEmptyBlockOnFocusLoss: true
    sourceText: editorView.blockModel && editorView.blockModel.markdown
                ? editorView.markdownForRendering(block.blockText) : block.blockText
    keyHandler: function(event) {
        // Keys.BeforeItem runs before TextArea inserts the space.  Arm
        // a zero-delay retry while the unspaced token is still intact;
        // the timer then sees the final "*tag " document after the
        // built-in key handler has completed.
        if (event.key === Qt.Key_Space
                && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                && selectionStart === selectionEnd && tagLineCandidate(true))
            scheduleTagLinePromotion(false)
        return editorView.handleHeadingShortcut(event, textCell)
    }
    property bool deferredTagLineForce: false

    function normalizedTagLinePlainText() {
        // QML TextEdit may expose QTextDocument paragraph/line
        // separators instead of an ASCII newline.  Normalize them
        // one-for-one so C++ line parsing and cursor offsets agree.
        return currentPlainText().replace(/\r\n/g, "\n")
                                 .replace(/[\r\u2028\u2029]/g, "\n")
    }

    function tagLineCandidate(force) {
        if (!activeFocus || syncingSourceText || !editorView.blockModel || !editorView.blockModel.markdown
                || editorView.blockModel.blockTypeAt(0) !== 0
                || (block.index !== 0 && block.index !== 1))
            return null
        const plainText = normalizedTagLinePlainText()
        let bodyText = plainText
        if (block.index === 0) {
            // The visual second paragraph is the first body line.  A
            // Markdown writer serializes the same boundary as a blank
            // source line.  Some Qt versions also expose that separator
            // in the plain projection, so tolerate empty lines here.
            const titleEnd = plainText.indexOf("\n")
            if (titleEnd < 0)
                return null
            bodyText = plainText.substring(titleEnd + 1)
        }
        const bodyLines = bodyText.split("\n")
        let candidate = ""
        for (const line of bodyLines) {
            if (line.trim().length > 0) {
                candidate = line
                break
            }
        }
        if (candidate.length === 0 || candidate.trim().charAt(0) !== "*"
                || (!force && !/\s$/.test(candidate)))
            return null
        return { plainText: plainText, candidate: candidate }
    }

    function scheduleTagLinePromotion(force) {
        deferredTagLineForce = deferredTagLineForce || Boolean(force)
        tagLinePromotionTimer.restart()
    }

    function tryPromoteTagLine(force) {
        const probe = tagLineCandidate(force)
        if (!probe)
            return false
        const markdownText = editorView.editorBackend.markdownText(textDocument)
        const position = cursorPosition
        let result = null
        editorView.runEditTransaction("promote-tag-line", function() {
            result = editorView.blockModel.promoteTagLineFromText(
                        block.index, probe.plainText, markdownText, position, Boolean(force))
            return Boolean(result && result.handled)
        }, false)
        if (!result || !result.handled)
            return false

        editorView.activeEditor = null
        if (result.focusTagLine) {
            editorView.activeTagLineIndex = Number(result.tagRow)
            Qt.callLater(function() {
                editorView.focusTagLineBlock(Number(result.tagRow), true, true)
            })
        } else {
            editorView.activeTagLineIndex = -1
            Qt.callLater(function() {
                editorView.focusBlock(Number(result.focusRow), false,
                                Number(result.cursorPosition))
            })
        }
        return true
    }

    Timer {
        id: tagLinePromotionTimer
        interval: 0
        repeat: false
        onTriggered: {
            const force = textCell.deferredTagLineForce
            textCell.deferredTagLineForce = false
            if (!textCell.tryPromoteTagLine(force))
                textCell.commitChangedText(textCell.activeFocus)
        }
    }
    commitText: function() {
        const value = editorView.blockModel && editorView.blockModel.markdown
            ? editorView.editorBackend.markdownText(textDocument) : text
        editorView.blockModel.setBlockText(block.index, value)
    }
    textFormat: editorView.blockModel && editorView.blockModel.markdown ? TextEdit.MarkdownText : TextEdit.PlainText
    onTextChanged: {
        // QTextDocument may still be normalizing the just-inserted
        // character while TextArea emits textChanged.  Replacing the
        // delegate/model synchronously from that signal can therefore
        // observe the pre-space document and miss "*tag ".  Defer only
        // plausible tag-line candidates to the next event-loop turn;
        // ordinary text keeps its existing immediate commit path.
        if (tagLineCandidate(false))
            scheduleTagLinePromotion(false)
        else
            commitChangedText(activeFocus)
    }
    onLinkActivated: link => Qt.openUrlExternally(link)
}
