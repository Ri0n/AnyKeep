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

    function statusColor() {
        const dark = paletteIsDark()
        if (urgencyState === "overdue")
            return dark ? Qt.rgba(0.973, 0.318, 0.286, 1) : Qt.rgba(0.812, 0.133, 0.180, 1)
        if (urgencyState === "soon")
            return dark ? Qt.rgba(0.824, 0.600, 0.133, 1) : Qt.rgba(0.604, 0.404, 0.000, 1)
        return dark ? Qt.rgba(0.247, 0.725, 0.314, 1) : Qt.rgba(0.102, 0.498, 0.216, 1)
    }

    function backgroundColor(hovered) {
        const base = statusColor()
        const dark = paletteIsDark()
        return Qt.rgba(base.r, base.g, base.b,
                       hovered ? (dark ? 0.24 : 0.16) : (dark ? 0.16 : 0.10))
    }

    function borderColor(hovered) {
        const base = statusColor()
        const dark = paletteIsDark()
        return Qt.rgba(base.r, base.g, base.b,
                       hovered ? (dark ? 0.72 : 0.55) : (dark ? 0.46 : 0.34))
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
    readonly property bool caretImmediatelyAfter: editor.activeFocus
                                                 && editor.selectionStart === editor.selectionEnd
                                                 && editor.cursorPosition === element.end

    Repeater {
        model: root.segments
        delegate: Rectangle {
            id: segmentBackground
            required property var modelData
            readonly property real leftExtra: root.editor.editorView.touchMode ? 2.5 : 2
            readonly property real rightExtra: root.caretImmediatelyAfter ? 0 : leftExtra
            x: modelData.x - leftExtra
            y: modelData.y + 1
            width: modelData.width + leftExtra + rightExtra
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
