import QtQuick
import QtQuick.Controls
import "../../reorder" as Reorder
import "../../shared" as Shared

// One visual card owns the poster, optional player, placeholder and controls.
// Its outer bounds are the only geometry used for alignment, resize and selection.
Rectangle {
    id: card
    required property var mediaBlock
    objectName: "mediaVideoCard-" + mediaBlock.block.index

    readonly property real posterImplicitWidth: poster.implicitWidth
    readonly property real posterImplicitHeight: poster.implicitHeight
    readonly property int transportHeight: mediaBlock.transportHeight
    readonly property bool hasPoster:
        String(mediaBlock.block.previewUrl).startsWith("image://anykeep-media/")
        || String(mediaBlock.block.previewUrl).startsWith("data:image/jpeg;base64,")

    color: mediaBlock.editorView.documentCardColor
    border.width: 1
    border.color: mediaBlock.editorView.documentCardBorderColor
    radius: 6
    clip: true

    function openFullScreen() {
        if (!mediaBlock.playback || !mediaBlock.playback.available)
            return
        videoSurface.fullScreenWhenReady = true
        if (!mediaBlock.current)
            mediaBlock.playback.play(mediaBlock.block.url)
        else if (videoSurface.item) {
            videoSurface.fullScreenWhenReady = false
            videoSurface.item.openFullScreen()
        }
    }

    Rectangle {
        id: videoFrame
        objectName: "mediaVideoFrame-" + mediaBlock.block.index
        x: card.border.width
        y: card.border.width
        width: Math.max(0, card.width - 2 * card.border.width)
        height: Math.max(40, card.height - transport.height - 2 * card.border.width)
        color: card.color
        clip: true

        Image {
            id: poster
            objectName: "mediaVideoPoster-" + mediaBlock.block.index
            anchors.fill: parent
            source: card.hasPoster ? mediaBlock.block.previewUrl : ""
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            cache: true
            visible: !(mediaBlock.current && videoSurface.status === Loader.Ready)
        }

        Shared.ThemedIconImpl {
            objectName: "mediaVideoPlaceholderGlyph-" + mediaBlock.block.index
            anchors.centerIn: parent
            visible: poster.status !== Image.Ready
                     && !(mediaBlock.current && videoSurface.status === Loader.Ready)
            themeName: "__bundled__"
            fallbackName: "video-x-generic-symbolic.svg"
            recolorFallback: true
            fallbackTintMode: String(mediaBlock.editorView.documentSecondaryTextColor)
            pixelSize: 32
            opacity: 0.6
        }

        Loader {
            id: videoSurface
            objectName: "mediaVideoOutputLoader-" + mediaBlock.block.index
            anchors.fill: parent
            active: mediaBlock.current && mediaBlock.playback && mediaBlock.playback.available
            source: active ? "MediaVideoSurface.qml" : ""
            z: 1
            property bool fullScreenWhenReady: false
            onLoaded: {
                item.playback = mediaBlock.playback
                if (fullScreenWhenReady) {
                    fullScreenWhenReady = false
                    item.openFullScreen()
                }
            }
        }

        HoverHandler { id: mediaHover }
        ToolTip.visible: mediaHover.hovered && mediaBlock.block.alt.length > 0 && !mediaBlock.selected
        ToolTip.text: mediaBlock.block.alt

        Reorder.ReorderDragHandle {
            anchors.fill: parent
            z: 2
            dragEnabled: !mediaBlock.editorView.touchMode && mediaBlock.transientWidth < 0
            hoverCursorShape: Qt.OpenHandCursor
            dragCursorShape: Qt.ClosedHandCursor
            onTapped: mediaBlock.selectAndFocus()
            onDragStarted: {
                mediaBlock.selectAndFocus()
                mediaBlock.reorderController.startBlockDrag(mediaBlock.block.parent, mediaBlock)
            }
            onDragMoved: function(dx, dy) {
                mediaBlock.reorderController.moveBlockDrag(dx, dy)
            }
            onDragFinished: mediaBlock.reorderController.finishBlockDrag()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.DragThreshold
            onTapped: mediaBlock.openMediaContextMenu()
        }
        TapHandler {
            enabled: mediaBlock.editorView.touchMode
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onLongPressed: mediaBlock.openMediaContextMenu()
        }
    }

    // The transport belongs to the card's layout, rather than floating above
    // the final part of the video. The frame keeps the true video aspect ratio.
    MediaTransportControls {
        id: transport
        mediaBlock: card.mediaBlock
        embedded: true
        x: card.border.width
        y: card.height - height - card.border.width
        width: Math.max(0, card.width - 2 * card.border.width)
        height: card.transportHeight
    }

    Rectangle {
        x: card.border.width
        y: transport.y
        width: transport.width
        height: 1
        color: card.border.color
        opacity: 0.45
    }
}
