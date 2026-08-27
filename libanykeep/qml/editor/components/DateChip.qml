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
            return hovered ? "#ef5350" : "#e53935"
        if (urgencyState === "soon")
            return hovered ? "#ffe082" : "#ffd54f"
        return hovered ? "#66bb6a" : "#43a047"
    }

    function borderColor() {
        if (urgencyState === "overdue")
            return "#b71c1c"
        if (urgencyState === "soon")
            return "#c59a17"
        return "#2e7d32"
    }

    function foregroundColor() {
        return urgencyState === "soon" ? "#1a1a1a" : "#ffffff"
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

    Repeater {
        model: root.segments
        delegate: Rectangle {
            id: segmentBackground
            required property var modelData
            // Stay inside the QTextDocument range. Extending a chip into the
            // neighbouring space hides both the visual gap and its caret.
            x: modelData.x
            y: modelData.y
            width: modelData.width
            height: Math.max(2, modelData.height)
            radius: Math.min(6, height / 3)
            color: root.backgroundColor(segmentMouse.containsMouse)
            border.color: root.borderColor()
            border.width: 1

            Text {
                anchors.fill: parent
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: root.editor.getText(parent.modelData.start, parent.modelData.end)
                font: root.editor.font
                // The layout width is still the ten source characters. A
                // slightly compact label creates real visual padding without
                // stealing width from surrounding Markdown whitespace.
                scale: root.editor.editorView.touchMode ? 0.94 : 0.92
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
