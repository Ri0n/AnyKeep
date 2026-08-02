import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: linkEditor
    required property var editorView
    parent: Overlay.overlay
    modal: false
    focus: !hoverMode
    padding: editorView.touchMode ? 12 : 8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: editorView.touchMode
           ? Math.max(160, parent ? parent.width - 24 : 360)
           : Math.min(420, Math.max(240, parent ? parent.width - 16 : 420))

    property var editor: null
    property int selectionStart: 0
    property int selectionEnd: 0
    property bool hadLink: false
    property bool hoverMode: false
    property bool pointerInside: false

    Timer {
        id: hoverLinkCloseTimer
        interval: 250
        onTriggered: {
            if (!linkEditor.hoverMode || linkEditor.pointerInside)
                return
            if (linkEditor.editor && linkEditor.editor.linkHoverActive)
                return
            linkEditor.close()
        }
    }

    function openFor(target, start, end, href, activate) {
        hoverLinkCloseTimer.stop()
        editor = target
        selectionStart = start
        selectionEnd = end
        urlField.text = href || ""
        hadLink = urlField.text.length > 0
        hoverMode = activate === false
        const rectangle = target.positionToRectangle(start)
        const point = target.mapToItem(Overlay.overlay, rectangle.x, rectangle.y)
        const inset = editorView.touchMode ? 12 : 8
        x = Math.max(inset, Math.min(Overlay.overlay.width - width - inset, point.x))
        y = Math.max(inset, point.y - implicitHeight - 6)
        open()
        if (!hoverMode) {
            Qt.callLater(function() {
                urlField.forceActiveFocus()
                urlField.selectAll()
            })
        }
    }

    function scheduleHoverClose(target) {
        if (hoverMode && editor === target)
            hoverLinkCloseTimer.restart()
    }

    function applyLink(href) {
        if (!editor)
            return
        editorView.runEditTransaction("set-link", function() {
            const end = editorView.editorBackend.setLink(editor.textDocument,
                                              selectionStart,
                                              selectionEnd,
                                              href.trim())
            if (end >= 0) {
                editor.select(selectionStart, end)
                editor.commitText(false)
            }
        })
        close()
        editor.forceActiveFocus()
    }

    onClosed: {
        hoverLinkCloseTimer.stop()
        pointerInside = false
        if (!hoverMode && editor)
            editor.forceActiveFocus()
        hoverMode = false
    }

    contentItem: GridLayout {
        columns: editorView.touchMode ? 2 : 3
        columnSpacing: editorView.touchMode ? 8 : 6
        rowSpacing: editorView.touchMode ? 8 : 6

        HoverHandler {
            onHoveredChanged: {
                linkEditor.pointerInside = hovered
                if (hovered)
                    hoverLinkCloseTimer.stop()
                else if (linkEditor.hoverMode)
                    hoverLinkCloseTimer.restart()
            }
        }

        TextField {
            id: urlField
            objectName: "noteLinkUrlField"
            Layout.columnSpan: editorView.touchMode ? 2 : 1
            Layout.fillWidth: true
            placeholderText: qsTr("Paste or type a link")
            selectByMouse: true
            font: editorView.editorFont
            onActiveFocusChanged: {
                if (activeFocus) {
                    linkEditor.hoverMode = false
                    hoverLinkCloseTimer.stop()
                }
            }
            onAccepted: linkEditor.applyLink(text)
        }
        Button {
            text: qsTr("Apply")
            Layout.fillWidth: editorView.touchMode
            Layout.minimumHeight: editorView.touchMode ? 44 : 0
            onClicked: linkEditor.applyLink(urlField.text)
        }
        ToolButton {
            text: qsTr("Remove")
            Layout.fillWidth: editorView.touchMode
            Layout.minimumHeight: editorView.touchMode ? 44 : 0
            enabled: linkEditor.hadLink
            onClicked: linkEditor.applyLink("")
        }
    }
}
