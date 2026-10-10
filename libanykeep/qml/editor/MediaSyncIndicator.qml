import QtQuick
import QtQuick.Controls

// A compact, non-interactive, provider-neutral indicator. The arc represents
// verified offline bytes; rotation occurs ONLY during actual transfer.
// Pause/error/complete have distinct shapes as well as palette-aware colors.
Item {
    id: root
    objectName: "mediaSyncIndicator"
    property real progress: 0
    property int syncState: 0 // Waiting, Transferring, Failed, Complete
    property bool noteWide: false
    property string errorString: ""
    readonly property real fraction: syncState === 3 ? 1 : Math.max(0, Math.min(1, progress))
    readonly property string statusText: {
        const prefix = noteWide ? qsTr("Note media") : qsTr("Media")
        if (syncState === 3)
            return prefix + qsTr(": available offline")
        if (syncState === 2)
            return prefix + qsTr(": synchronization failed")
        if (syncState === 1)
            return prefix + qsTr(": downloading")
        return prefix + qsTr(": waiting for connection or paused")
    }

    implicitWidth: 24
    implicitHeight: 24
    Accessible.role: Accessible.StaticText
    Accessible.name: root.statusText
    Accessible.description: root.syncState === 2 && root.errorString.length > 0
                            ? root.errorString : root.statusText
    SystemPalette { id: theme }

    Canvas {
        id: progressArc
        anchors.centerIn: parent
        width: Math.min(root.width, root.height, 24)
        height: width
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const cx = width / 2
            const cy = height / 2
            const radius = Math.max(1, width / 2 - 3)
            ctx.lineWidth = 2.4
            ctx.lineCap = "round"
            ctx.strokeStyle = theme.mid
            ctx.beginPath()
            ctx.arc(cx, cy, radius, 0, 2 * Math.PI)
            ctx.stroke()
            if (root.fraction > 0) {
                ctx.strokeStyle = root.syncState === 2 ? theme.text
                                : root.syncState === 0 ? theme.windowText
                                : theme.highlight
                ctx.beginPath()
                ctx.arc(cx, cy, radius, -Math.PI / 2,
                        -Math.PI / 2 + 2 * Math.PI * root.fraction)
                ctx.stroke()
            }
        }
        Connections {
            target: root
            function onFractionChanged() { progressArc.requestPaint() }
            function onSyncStateChanged() { progressArc.requestPaint() }
        }
        Connections {
            target: theme
            function onPaletteChanged() { progressArc.requestPaint() }
        }
        RotationAnimator {
            target: progressArc
            from: 0
            to: 360
            duration: 1600
            loops: Animation.Infinite
            running: root.syncState === 1
            onRunningChanged: {
                if (!running)
                    progressArc.rotation = 0
            }
        }
    }

    Canvas {
        id: stateGlyph
        anchors.centerIn: parent
        width: Math.min(root.width, root.height, 24)
        height: width
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const c = width / 2
            const unit = width / 24
            ctx.lineWidth = 2 * unit
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.strokeStyle = root.syncState === 2 ? theme.text : theme.windowText
            ctx.beginPath()
            if (root.syncState === 0) {
                ctx.moveTo(c - 2.4 * unit, c - 3 * unit)
                ctx.lineTo(c - 2.4 * unit, c + 3 * unit)
                ctx.moveTo(c + 2.4 * unit, c - 3 * unit)
                ctx.lineTo(c + 2.4 * unit, c + 3 * unit)
            } else if (root.syncState === 2) {
                ctx.moveTo(c - 2.6 * unit, c - 2.6 * unit)
                ctx.lineTo(c + 2.6 * unit, c + 2.6 * unit)
                ctx.moveTo(c + 2.6 * unit, c - 2.6 * unit)
                ctx.lineTo(c - 2.6 * unit, c + 2.6 * unit)
            } else if (root.syncState === 3) {
                ctx.moveTo(c - 3.4 * unit, c)
                ctx.lineTo(c - 0.6 * unit, c + 2.6 * unit)
                ctx.lineTo(c + 3.7 * unit, c - 3 * unit)
            } else {
                ctx.arc(c, c, 1.6 * unit, 0, 2 * Math.PI)
            }
            ctx.stroke()
        }
        Connections {
            target: root
            function onSyncStateChanged() { stateGlyph.requestPaint() }
        }
        Connections {
            target: theme
            function onPaletteChanged() { stateGlyph.requestPaint() }
        }
    }

    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.text: root.syncState === 2 && root.errorString.length > 0
                  ? root.statusText + ": " + root.errorString : root.statusText
}
