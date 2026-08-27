import QtQuick

Item {
    id: root
    required property var editor
    required property var element
    required property string urgencyState
    required property int layoutRevision
    signal activated()

    anchors.fill: parent

    function backgroundColor(hovered) {
        if (urgencyState === "overdue")
            return hovered ? "#f27d7d" : "#ef6c6c"
        if (urgencyState === "soon")
            return hovered ? "#ffe082" : "#ffd54f"
        return hovered ? "#93d497" : "#81c784"
    }

    function borderColor() {
        if (urgencyState === "overdue")
            return "#b94343"
        if (urgencyState === "soon")
            return "#c59a17"
        return "#4f8f58"
    }

    function foregroundColor() {
        return "#1a1a1a"
    }

    function segmentsForRange() {
        // These reads make the binding follow layout-affecting changes that do
        // not alter the source range itself.
        const revision = layoutRevision
        const editorWidth = editor.width
        const editorHeight = editor.height
        const fontPointSize = editor.font.pointSize
        void revision
        void editorWidth
        void editorHeight
        void fontPointSize

        if (!element || element.start < 0 || element.end <= element.start
                || element.end > editor.length)
            return []

        const averageWidth = editor.editorView
                ? editor.editorView.editorFontAverageCharacterWidth : 8
        const segments = []
        let segment = null
        for (let position = element.start; position < element.end; ++position) {
            const current = editor.positionToRectangle(position)
            const next = editor.positionToRectangle(position + 1)
            const sameLine = Math.abs(next.y - current.y) < 0.5
            const right = sameLine && next.x > current.x
                    ? next.x
                    : current.x + Math.max(current.width, averageWidth)

            if (!segment || Math.abs(segment.y - current.y) >= 0.5) {
                if (segment)
                    segments.push(segment)
                segment = {
                    x: current.x,
                    y: current.y,
                    right: right,
                    height: current.height,
                    start: position,
                    end: position + 1
                }
            } else {
                segment.right = Math.max(segment.right, right)
                segment.height = Math.max(segment.height, current.height)
                segment.end = position + 1
            }

            if (!sameLine || position === element.end - 1) {
                segments.push(segment)
                segment = null
            }
        }
        if (segment)
            segments.push(segment)

        for (const value of segments)
            value.width = Math.max(1, value.right - value.x)
        return segments
    }

    readonly property var segments: segmentsForRange()
    readonly property bool caretImmediatelyAfter: editor.activeFocus
                                                 && editor.selectionStart === editor.selectionEnd
                                                 && editor.cursorPosition === element.end

    Repeater {
        model: root.segments
        delegate: Rectangle {
            id: segmentBackground
            required property var modelData
            readonly property real horizontalPadding: root.editor.editorView.touchMode ? 4 : 3.5
            readonly property real rightPadding: root.caretImmediatelyAfter ? 0 : horizontalPadding
            x: modelData.x - horizontalPadding
            y: modelData.y
            width: modelData.width + horizontalPadding + rightPadding
            height: Math.max(2, modelData.height)
            radius: Math.min(6, height / 3)
            color: root.backgroundColor(segmentMouse.containsMouse)
            border.color: root.borderColor()
            border.width: 1

            Text {
                anchors.fill: parent
                leftPadding: segmentBackground.horizontalPadding
                rightPadding: segmentBackground.rightPadding
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: root.editor.getText(parent.modelData.start, parent.modelData.end)
                font: root.editor.font
                color: root.foregroundColor()
                elide: Text.ElideNone
            }

            MouseArea {
                id: segmentMouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.activated()
            }
        }
    }
}
