import QtQuick
import QtQuick.Controls
import "../.." as EditorComponents
import "../../reorder" as Reorder
import "../../shared" as Shared

// One visual card owns the poster, optional player, placeholder and controls.
// Its outer bounds are the only geometry used for alignment, resize and selection.
Rectangle {
    id: card
    required property var mediaRoot
    objectName: "mediaVideoCard-" + mediaRoot.block.index

    readonly property real posterImplicitWidth: poster.implicitWidth
    readonly property real posterImplicitHeight: poster.implicitHeight
    readonly property int transportHeight: mediaRoot.transportHeight
    readonly property bool hasPoster:
        String(mediaRoot.block.previewUrl).startsWith("image://anykeep-media/")
        || String(mediaRoot.block.previewUrl).startsWith("data:image/jpeg;base64,")

    color: mediaRoot.editorView.documentCardColor
    border.width: 1
    border.color: mediaRoot.editorView.documentCardBorderColor
    radius: 6
    clip: true

    function openFullScreen() {
        if (!mediaRoot.playback || !mediaRoot.playback.available)
            return
        videoSurface.fullScreenWhenReady = true
        if (!mediaRoot.current)
            mediaRoot.playback.play(mediaRoot.block.url)
        else if (videoSurface.item) {
            videoSurface.fullScreenWhenReady = false
            videoSurface.item.openFullScreen()
        }
    }

    Rectangle {
        id: videoFrame
        objectName: "mediaVideoFrame-" + mediaRoot.block.index
        x: card.border.width
        y: card.border.width
        width: Math.max(0, card.width - 2 * card.border.width)
        height: Math.max(40, card.height - transport.height - 2 * card.border.width)
        color: card.color
        clip: true

        Image {
            id: poster
            objectName: "mediaVideoPoster-" + mediaRoot.block.index
            anchors.fill: parent
            source: card.hasPoster ? mediaRoot.block.previewUrl : ""
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            cache: true
            visible: !(mediaRoot.current && videoSurface.status === Loader.Ready)
        }

        Shared.ThemedIconImpl {
            objectName: "mediaVideoPlaceholderGlyph-" + mediaRoot.block.index
            anchors.centerIn: parent
            visible: poster.status !== Image.Ready
                     && !(mediaRoot.current && videoSurface.status === Loader.Ready)
            themeName: "__bundled__"
            fallbackName: "video-x-generic-symbolic.svg"
            recolorFallback: true
            fallbackTintMode: String(mediaRoot.editorView.documentSecondaryTextColor)
            pixelSize: 32
            opacity: 0.6
        }

        Loader {
            id: videoSurface
            objectName: "mediaVideoOutputLoader-" + mediaRoot.block.index
            anchors.fill: parent
            active: mediaRoot.current && mediaRoot.playback && mediaRoot.playback.available
            source: active ? "MediaVideoSurface.qml" : ""
            z: 1
            property bool fullScreenWhenReady: false
            onLoaded: {
                item.playback = mediaRoot.playback
                if (fullScreenWhenReady) {
                    fullScreenWhenReady = false
                    item.openFullScreen()
                }
            }
        }

        HoverHandler { id: mediaHover }
        ToolTip.visible: mediaHover.hovered && mediaRoot.block.alt.length > 0 && !mediaRoot.selected
        ToolTip.text: mediaRoot.block.alt

        Reorder.ReorderDragHandle {
            anchors.fill: parent
            z: 2
            dragEnabled: !mediaRoot.editorView.touchMode && mediaRoot.transientWidth < 0
            hoverCursorShape: Qt.OpenHandCursor
            dragCursorShape: Qt.ClosedHandCursor
            onTapped: mediaRoot.selectAndFocus()
            onDragStarted: {
                mediaRoot.selectAndFocus()
                mediaRoot.reorderController.startBlockDrag(mediaRoot.block.parent, mediaRoot)
            }
            onDragMoved: function(dx, dy) {
                mediaRoot.reorderController.moveBlockDrag(dx, dy)
            }
            onDragFinished: mediaRoot.reorderController.finishBlockDrag()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.DragThreshold
            onTapped: mediaRoot.openMediaContextMenu()
        }
        TapHandler {
            enabled: mediaRoot.editorView.touchMode
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onLongPressed: mediaRoot.openMediaContextMenu()
        }
    }

    // The transport belongs to the card's layout, rather than floating above
    // the final part of the video. The frame keeps the true video aspect ratio.
    MediaTransportControls {
        id: transport
        mediaRoot: card.mediaRoot
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
