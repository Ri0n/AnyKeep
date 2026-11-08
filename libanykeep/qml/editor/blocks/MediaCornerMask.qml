import QtQuick

// Rectangle.clip is rectangular. Restore only the four corner pixels above
// poster/video/control children, using the owning card's radius and border.
Item {
    id: mask
    required property real cornerRadius
    required property real borderWidth
    required property color borderColor
    required property color outsideColor
    Repeater {
        model: 4
        delegate: Canvas {
            required property int index
            width: mask.cornerRadius
            height: width
            x: index === 1 || index === 2 ? mask.width : 0
            y: index >= 2 ? mask.height : 0
            rotation: index * 90
            transformOrigin: Item.TopLeft
            Connections {
                target: mask
                function onCornerRadiusChanged() { requestPaint() }
                function onBorderWidthChanged() { requestPaint() }
                function onBorderColorChanged() { requestPaint() }
                function onOutsideColorChanged() { requestPaint() }
            }
            onWidthChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                const r = width
                const inner = Math.max(0, r - mask.borderWidth)
                ctx.clearRect(0, 0, r, r)
                ctx.globalCompositeOperation = "source-over"
                ctx.fillStyle = mask.outsideColor
                ctx.fillRect(0, 0, r, r)
                ctx.fillStyle = mask.borderColor
                ctx.beginPath()
                ctx.moveTo(r, r)
                ctx.lineTo(r, 0)
                ctx.arc(r, r, r, -Math.PI / 2, -Math.PI, true)
                ctx.closePath()
                ctx.fill()
                ctx.globalCompositeOperation = "destination-out"
                ctx.beginPath()
                ctx.moveTo(r, r)
                ctx.lineTo(r, r - inner)
                ctx.arc(r, r, inner, -Math.PI / 2, -Math.PI, true)
                ctx.closePath()
                ctx.fill()
                ctx.globalCompositeOperation = "source-over"
            }
        }
    }
}
