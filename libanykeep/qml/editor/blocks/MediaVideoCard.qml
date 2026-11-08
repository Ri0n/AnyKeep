import QtQuick
import QtQuick.Controls
import "../../shared" as Shared

// One visual card owns the poster, optional player, placeholder and controls.
// Its outer bounds are the only geometry used for alignment, resize and selection.
Rectangle {
    id: card
    required property var mediaBlock
    objectName: "mediaVideoCard-" + mediaBlock.block.index

    readonly property real posterImplicitWidth: poster.implicitWidth
    readonly property real posterImplicitHeight: poster.implicitHeight
    readonly property real titleAreaHeight: titleOverlay.visible ? titleOverlay.y + titleOverlay.height + 4 : 0
    readonly property int transportHeight: mediaBlock.transportHeight
    readonly property bool hasPoster:
        String(mediaBlock.block.previewUrl).startsWith("image://anykeep-media/")
        || String(mediaBlock.block.previewUrl).startsWith("data:image/jpeg;base64,")
    readonly property bool showingVideo: mediaBlock.current && mediaBlock.playback
                                         && mediaBlock.playback.hasVideoFrame
                                         && videoSurface.status === Loader.Ready

    color: mediaBlock.editorView.documentCardColor
    border.width: mediaBlock.selected ? 2 : 1
    border.color: mediaBlock.selected ? mediaBlock.selectionColor : mediaBlock.editorView.documentCardBorderColor
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
            visible: !card.showingVideo
        }

        Shared.ThemedIconImpl {
            objectName: "mediaVideoPlaceholderGlyph-" + mediaBlock.block.index
            anchors.centerIn: parent
            visible: poster.status !== Image.Ready
                     && !card.showingVideo
            themeName: "__bundled__"
            fallbackName: "video-x-generic-symbolic.svg"
            recolorFallback: true
            fallbackTintMode: String(mediaBlock.editorView.documentSecondaryTextColor)
            pixelSize: 64
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

        Rectangle {
            id: titleOverlay
            objectName: "mediaVideoTitleOverlay-" + mediaBlock.block.index
            x: 8
            y: 8
            width: Math.max(0, parent.width - 16)
            height: titleField.implicitHeight + 4
            radius: 4
            color: "#a0000000"
            visible: !mediaBlock.playing || titleField.editing
            z: 3
            MediaTitleField {
                id: titleField
                mediaBlock: card.mediaBlock
                anchors.fill: parent
                anchors.margins: 2
            }
        }
    }

    Connections {
        target: videoSurface.item
        function onFullScreenChanged() {
            card.mediaBlock.editorView.mediaFullScreen = videoSurface.item.fullScreen
        }
    }
    Component.onDestruction: {
        if (videoSurface.item && videoSurface.item.fullScreen)
            mediaBlock.editorView.mediaFullScreen = false
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

    MediaCornerMask {
        anchors.fill: parent
        z: 10
        cornerRadius: card.radius
        borderWidth: card.border.width
        borderColor: card.border.color
        outsideColor: mediaBlock.editorView.documentBackgroundColor
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
