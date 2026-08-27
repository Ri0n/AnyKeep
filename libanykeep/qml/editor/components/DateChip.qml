import QtQuick

Item {
    id: root
    required property var editor
    required property var element
    required property string urgencyState
    required property int layoutRevision
    signal activated()

    anchors.fill: parent

    function paletteIsDark() {
        const base = editor.palette.base
        return 0.299 * base.r + 0.587 * base.g + 0.114 * base.b < 0.5
    }

    function backgroundColor(hovered) {
        const dark = paletteIsDark()
        const alpha = hovered ? (dark ? 0.42 : 0.34) : (dark ? 0.30 : 0.23)
        if (urgencyState === "overdue")
            return Qt.rgba(dark ? 0.95 : 0.82, dark ? 0.31 : 0.16, dark ? 0.31 : 0.14, alpha)
        if (urgencyState === "soon")
            return Qt.rgba(dark ? 0.96 : 0.93, dark ? 0.72 : 0.61, dark ? 0.20 : 0.08, alpha)
        return Qt.rgba(dark ? 0.32 : 0.12, dark ? 0.78 : 0.62, dark ? 0.43 : 0.25, alpha)
    }

    function borderColor(hovered) {
        const dark = paletteIsDark()
        const alpha = hovered ? 0.72 : 0.48
        if (urgencyState === "overdue")
            return Qt.rgba(dark ? 1.0 : 0.72, dark ? 0.42 : 0.12, dark ? 0.42 : 0.10, alpha)
        if (urgencyState === "soon")
            return Qt.rgba(dark ? 1.0 : 0.78, dark ? 0.80 : 0.52, dark ? 0.30 : 0.04, alpha)
        return Qt.rgba(dark ? 0.43 : 0.08, dark ? 0.88 : 0.52, dark ? 0.55 : 0.20, alpha)
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
                    height: current.height
                }
            } else {
                segment.right = Math.max(segment.right, right)
                segment.height = Math.max(segment.height, current.height)
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
            x: modelData.x - 3
            y: modelData.y + 1
            width: modelData.width + 6
            height: Math.max(2, modelData.height - 2)
            radius: Math.min(6, height / 3)
            color: root.backgroundColor(segmentMouse.containsMouse)
            border.color: root.borderColor(segmentMouse.containsMouse)
            border.width: 1

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
