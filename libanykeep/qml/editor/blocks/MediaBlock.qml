import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../reorder" as Reorder
import "../../shared" as Shared

FocusScope {
    id: mediaRoot
    required property var editorView
    required property var reorderController
    objectName: "mediaBlockEditor-" + block.index
    required property var block
    property var playback: mediaRoot.editorView.editorBackend ? mediaRoot.editorView.editorBackend.mediaPlayback : null
    property var transcription: mediaRoot.editorView.audioTranscriptionController
    property bool transcriptExpanded: false
    readonly property bool timed: block.mediaType.startsWith("audio/") || block.mediaType.startsWith("video/")
    readonly property bool video: block.mediaType.startsWith("video/")
    readonly property bool audio: block.mediaType.startsWith("audio/")
    readonly property bool canTranscribe: audio && transcription && transcription.audioTranscriptionAvailable
    readonly property bool transcribing: canTranscribe && transcription.transcribingAudioRow === block.index
    readonly property bool transcriptionBlocked: transcription && transcription.busy && !transcribing
    readonly property bool current: playback && playback.currentSourceUri === block.url
    readonly property bool playing: current && playback.playing
    readonly property bool loading: current && playback.loading
    readonly property real knownDuration: current && playback.duration > 0 ? playback.duration : block.mediaDuration
    readonly property bool visual: block.mediaType.startsWith("image/") || block.mediaType.startsWith("video/")
    readonly property bool individuallySelected:
        mediaRoot.editorView.selectedMediaIndex === block.index
    readonly property bool selected: individuallySelected || block.structurallySelected
    property real transientWidth: -1
    property real transientX: -1
    property real resizeStartWidth: 0
    property real resizeStartX: 0
    property int resizeDirection: 1
    readonly property string normalizedAlignment:
        block.mediaAlignment === "left" || block.mediaAlignment === "right"
        ? block.mediaAlignment : "center"
    readonly property real naturalWidth:
        sourceImage.implicitWidth > 0 ? sourceImage.implicitWidth : Math.min(width, 480)
    readonly property real requestedWidth:
        transientWidth >= 0 ? transientWidth
                            : (block.mediaDisplayWidth > 0 ? block.mediaDisplayWidth : naturalWidth)
    readonly property real displayWidth: Math.max(1, Math.min(width, requestedWidth))
    readonly property real visualAspect:
        sourceImage.implicitWidth > 0 && sourceImage.implicitHeight > 0
        ? sourceImage.implicitHeight / sourceImage.implicitWidth
        : (block.mediaWidth > 0 && block.mediaHeight > 0
           ? block.mediaHeight / block.mediaWidth : 0.75)
    readonly property real displayHeight: visual ? Math.max(40, displayWidth * visualAspect) : 58
    readonly property real imageY: individuallySelected ? altEditor.implicitHeight + 6 : 0
    readonly property real actionGap: mediaRoot.editorView.touchMode ? 12 : 8
    readonly property real alignedX:
        normalizedAlignment === "left" ? 0
        : normalizedAlignment === "right" ? width - displayWidth
        : (width - displayWidth) / 2
    readonly property real displayX: transientX >= 0 ? transientX : alignedX

    readonly property real presentationActionsHeight:
        visual && individuallySelected ? actionGap + imageActions.height + 4 : 0
    readonly property real baseContentHeight: imageY + displayHeight + presentationActionsHeight

    width: block.width
    implicitHeight: baseContentHeight
                    + (transcriptPanel.visible ? 6 + transcriptPanel.implicitHeight : 0)
    activeFocusOnTab: true

    function selectAndFocus() {
        mediaRoot.editorView.selectMediaBlock(block.index)
        mediaRoot.forceActiveFocus()
    }

    function setAlignment(value) {
        if (normalizedAlignment === value)
            return
        mediaRoot.editorView.runEditTransaction("align-media", function() {
            mediaRoot.editorView.blockModel.setMediaAlignment(block.index, value)
        })
    }

    function resetPresentation() {
        if (block.mediaDisplayWidth === 0 && normalizedAlignment === "center")
            return
        mediaRoot.editorView.runEditTransaction("reset-media-presentation", function() {
            mediaRoot.editorView.blockModel.setMediaDisplayWidth(block.index, 0)
            mediaRoot.editorView.blockModel.setMediaAlignment(block.index, "center")
        })
    }

    function beginResize(direction) {
        resizeDirection = direction
        resizeStartWidth = displayWidth
        resizeStartX = sourceImage.x
        transientWidth = displayWidth
        transientX = sourceImage.x
        mediaRoot.editorView.beginEditTransaction("resize-media")
    }

    function updateResize(delta) {
        if (transientWidth < 0)
            return
        const maximum = resizeDirection > 0 ? width - resizeStartX
                                            : resizeStartX + resizeStartWidth
        const minimum = Math.min(mediaRoot.editorView.touchMode ? 72 : 48, maximum)
        if (resizeDirection > 0) {
            transientWidth = Math.max(minimum, Math.min(maximum, resizeStartWidth + delta))
            transientX = resizeStartX
        } else {
            transientWidth = Math.max(minimum, Math.min(maximum, resizeStartWidth - delta))
            transientX = resizeStartX + resizeStartWidth - transientWidth
        }
    }

    function finishResize() {
        if (transientWidth < 0)
            return
        const width = Math.round(transientWidth)
        mediaRoot.editorView.blockModel.setMediaDisplayWidth(block.index, width)
        transientWidth = -1
        transientX = -1
        mediaRoot.editorView.endEditTransaction()
    }

    function beginMarginSelection(area, mouse) {
        const point = area.mapToItem(mediaRoot.editorView, mouse.x, mouse.y)
        mediaRoot.editorView.beginBlankAreaSelection(block.index + 1, point.x, point.y)
        mouse.accepted = true
    }

    function updateMarginSelection(area, mouse) {
        if (!(mouse.buttons & Qt.LeftButton))
            return
        const point = area.mapToItem(mediaRoot.editorView, mouse.x, mouse.y)
        mediaRoot.editorView.updateBlankAreaSelection(point.x, point.y)
    }

    function finishMarginSelection(mouse) {
        if (mouse.button !== Qt.LeftButton)
            return
        if (!mediaRoot.editorView.finishBlankAreaSelection())
            selectAndFocus()
    }

    function requestTranscription() {
        selectAndFocus()
        if (!canTranscribe || transcriptionBlocked || transcribing)
            return false
        return transcription.transcribeAudio(block.index, block.url, block.mediaDuration)
    }

    function formatTime(milliseconds) {
        const seconds = Math.max(0, Math.floor(Number(milliseconds) / 1000))
        const remainder = seconds % 60
        return Math.floor(seconds / 60) + ":" + (remainder < 10 ? "0" : "") + remainder
    }

    Keys.onPressed: function(event) {
        if (altEditor.activeFocus)
            return
        if (timed && !event.modifiers && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            if (playback)
                playback.toggle(block.url)
            event.accepted = true
            return
        }
        if (timed && !event.modifiers && event.key === Qt.Key_Left && playback) {
            playback.seek(block.url, Math.max(0, (current ? playback.position : 0) - 5000))
            event.accepted = true
            return
        }
        if (timed && !event.modifiers && event.key === Qt.Key_Right && playback) {
            playback.seek(block.url, Math.min(knownDuration, (current ? playback.position : 0) + 5000))
            event.accepted = true
            return
        }
        if (event.matches(StandardKey.Copy)) {
            event.accepted = mediaRoot.editorView.copyActiveSelection()
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            mediaRoot.editorView.removeMediaBlock(block.index, true)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            mediaRoot.editorView.clearMediaSelection()
            mediaRoot.editorView.forceActiveFocus()
            event.accepted = true
        }
    }

    TextField {
        id: altEditor
        objectName: "mediaTitleEditor-" + mediaRoot.block.index
        visible: mediaRoot.individuallySelected
        opacity: visible ? 1 : 0
        width: Math.min(mediaRoot.width, Math.max(180, sourceImage.width))
        x: Math.max(0, Math.min(mediaRoot.width - width,
                                sourceImage.x + (sourceImage.width - width) / 2))
        placeholderText: mediaRoot.audio || mediaRoot.video ? qsTr("Media title") : qsTr("Alt text")
        text: mediaRoot.block.alt
        selectByMouse: true
        onTextEdited: mediaRoot.editorView.blockModel.setMediaTitle(mediaRoot.block.index, text)
        onActiveFocusChanged: mediaRoot.editorView.mediaTitleEditorFocused = activeFocus
        Component.onDestruction: {
            if (mediaRoot.editorView.mediaTitleEditorFocused)
                mediaRoot.editorView.mediaTitleEditorFocused = false
        }
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }

    Image {
        id: sourceImage
        objectName: "imageBlockPreview-" + mediaRoot.block.index
        x: mediaRoot.displayX
        y: mediaRoot.imageY
        width: mediaRoot.displayWidth
        height: mediaRoot.displayHeight
        source: mediaRoot.block.mediaType.startsWith("image/") ? mediaRoot.block.previewUrl : ""
        visible: mediaRoot.visual && !(mediaRoot.video && mediaRoot.current && videoSurface.status === Loader.Ready)
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        cache: true
        ToolTip.visible: imageDescriptionHover.hovered && mediaRoot.block.alt.length > 0
                             && !mediaRoot.selected
        ToolTip.text: mediaRoot.block.alt

        Behavior on x {
            enabled: mediaRoot.transientWidth < 0
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

        HoverHandler { id: imageDescriptionHover }
        Reorder.ReorderDragHandle {
            anchors.fill: parent
            dragEnabled: !mediaRoot.editorView.touchMode && mediaRoot.transientWidth < 0
            hoverCursorShape: Qt.OpenHandCursor
            dragCursorShape: Qt.ClosedHandCursor
            onTapped: mediaRoot.selectAndFocus()
            onDragStarted: {
                mediaRoot.selectAndFocus()
                mediaRoot.reorderController.startBlockDrag(
                            mediaRoot.block.parent, mediaRoot)
            }
            onDragMoved: function(dx, dy) {
                mediaRoot.reorderController.moveBlockDrag(dx, dy)
            }
            onDragFinished: mediaRoot.reorderController.finishBlockDrag()
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.DragThreshold
            onTapped: {
                mediaRoot.selectAndFocus()
                mediaContextMenu.popup()
            }
        }
        TapHandler {
            enabled: mediaRoot.editorView.touchMode
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onLongPressed: {
                mediaRoot.selectAndFocus()
                mediaContextMenu.popup()
            }
        }
    }

    Loader {
        id: videoSurface
        active: mediaRoot.video && mediaRoot.current
                && mediaRoot.playback && mediaRoot.playback.available
        source: active ? "MediaVideoSurface.qml" : ""
        x: sourceImage.x
        y: sourceImage.y
        width: sourceImage.width
        height: sourceImage.height
        z: 2
        property bool fullScreenWhenReady: false

        onLoaded: {
            item.playback = mediaRoot.playback
            if (fullScreenWhenReady) {
                fullScreenWhenReady = false
                item.openFullScreen()
            }
        }
    }

    // The image itself owns the reorder gesture, but the unused row
    // width is still part of the document. Let an upward drag from
    // either horizontal margin start selection at the boundary after
    // the image (especially important when it is the final block).
    MouseArea {
        id: imageLeftMarginSelectionArea
        x: 0
        y: sourceImage.y
        width: Math.max(0, sourceImage.x)
        height: sourceImage.height
        z: 1
        enabled: !mediaRoot.editorView.touchMode && !mediaRoot.reorderController.dragging && width > 0
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) { mediaRoot.beginMarginSelection(imageLeftMarginSelectionArea, mouse) }
        onPositionChanged: function(mouse) { mediaRoot.updateMarginSelection(imageLeftMarginSelectionArea, mouse) }
        onReleased: function(mouse) { mediaRoot.finishMarginSelection(mouse) }
        onCanceled: mediaRoot.editorView.cancelBlankAreaSelection()
    }

    MouseArea {
        id: imageRightMarginSelectionArea
        x: sourceImage.x + sourceImage.width
        y: sourceImage.y
        width: Math.max(0, mediaRoot.width - x)
        height: sourceImage.height
        z: 1
        enabled: !mediaRoot.editorView.touchMode && !mediaRoot.reorderController.dragging && width > 0
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) { mediaRoot.beginMarginSelection(imageRightMarginSelectionArea, mouse) }
        onPositionChanged: function(mouse) { mediaRoot.updateMarginSelection(imageRightMarginSelectionArea, mouse) }
        onReleased: function(mouse) { mediaRoot.finishMarginSelection(mouse) }
        onCanceled: mediaRoot.editorView.cancelBlankAreaSelection()
    }

    Rectangle {
        id: timedControls
        objectName: "mediaCard-" + mediaRoot.block.index
        visible: mediaRoot.timed
        x: mediaRoot.visual ? sourceImage.x : 0
        y: mediaRoot.visual ? sourceImage.y + sourceImage.height - height : mediaRoot.imageY
        width: mediaRoot.visual ? sourceImage.width : mediaRoot.width
        height: mediaRoot.visual ? (mediaRoot.editorView.touchMode ? 48 : 40) : mediaRoot.displayHeight
        color: mediaRoot.editorView.documentCardColor
        opacity: 0.94
        radius: 5
        z: 4

        RowLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 6
            ToolButton {
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                enabled: mediaRoot.playback && mediaRoot.playback.available
                text: mediaRoot.loading ? "…" : (mediaRoot.playing ? "Ⅱ" : "▶")
                Accessible.name: mediaRoot.playing ? qsTr("Pause media") : qsTr("Play media")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    mediaRoot.playback.toggle(mediaRoot.block.url)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    objectName: "mediaLabel-" + mediaRoot.block.index
                    Layout.fillWidth: true
                    visible: !mediaRoot.visual
                    text: mediaRoot.block.alt.length > 0 ? mediaRoot.block.alt : qsTr("Audio recording")
                    elide: Text.ElideMiddle
                    font.bold: mediaRoot.selected
                    color: mediaRoot.editorView.documentTextColor
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, mediaRoot.knownDuration)
                    value: mediaRoot.current ? mediaRoot.playback.position : 0
                    enabled: mediaRoot.playback && mediaRoot.knownDuration > 0
                    onMoved: mediaRoot.playback.seek(mediaRoot.block.url, value)
                }
            }
            Label {
                text: mediaRoot.formatTime(mediaRoot.current ? mediaRoot.playback.position : 0)
                      + " / " + mediaRoot.formatTime(mediaRoot.knownDuration)
                color: mediaRoot.editorView.documentSecondaryTextColor
            }
            ToolButton {
                visible: mediaRoot.canTranscribe
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                enabled: !mediaRoot.transcriptionBlocked && !mediaRoot.transcribing
                text: mediaRoot.transcribing ? "…" : "STT"
                Accessible.name: mediaRoot.block.mediaTranscript.length > 0
                                 ? qsTr("Show transcript") : qsTr("Transcribe audio")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    if (mediaRoot.block.mediaTranscript.length > 0)
                        mediaRoot.transcriptExpanded = !mediaRoot.transcriptExpanded
                    else
                        mediaRoot.requestTranscription()
                }
            }
            ToolButton {
                id: fullScreenMediaButton
                objectName: "mediaFullscreenButton-" + mediaRoot.block.index
                visible: mediaRoot.video
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                enabled: mediaRoot.playback && mediaRoot.playback.available
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "mediaFullscreenGlyph-" + mediaRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "view-fullscreen-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(fullScreenMediaButton.palette.buttonText)
                    pixelSize: 20
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Full screen")
                Accessible.name: qsTr("Show video full screen")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    videoSurface.fullScreenWhenReady = true
                    if (!mediaRoot.current)
                        mediaRoot.playback.play(mediaRoot.block.url)
                    else if (videoSurface.item) {
                        videoSurface.fullScreenWhenReady = false
                        videoSurface.item.openFullScreen()
                    }
                }
            }
            ToolButton {
                id: mediaActionsButton
                objectName: "mediaActionsButton-" + mediaRoot.block.index
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "mediaActionsGlyph-" + mediaRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "view-more-vertical-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(mediaActionsButton.palette.buttonText)
                    pixelSize: 20
                }
                Accessible.name: qsTr("Media actions")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    mediaContextMenu.popup()
                }
            }
        }
    }

    Rectangle {
        id: selectionOutline
        objectName: "mediaSelectionOutline-" + mediaRoot.block.index
        x: (mediaRoot.visual ? sourceImage.x : timedControls.x) - 2
        y: (mediaRoot.visual ? sourceImage.y : timedControls.y) - 2
        width: (mediaRoot.visual ? sourceImage.width : timedControls.width) + 4
        height: (mediaRoot.visual ? sourceImage.height : timedControls.height) + 4
        visible: mediaRoot.selected
        color: "transparent"
        border.width: 2
        border.color: altEditor.palette.highlight
        radius: 2
        z: 3
    }

    ImageResizeHandle {
        visible: mediaRoot.visual && mediaRoot.individuallySelected
        imageEditor: mediaRoot
        direction: -1
        fillColor: altEditor.palette.base
        strokeColor: altEditor.palette.highlight
        x: Math.max(0, sourceImage.x - width / 2)
        y: sourceImage.y + sourceImage.height - height / 2
    }

    ImageResizeHandle {
        visible: mediaRoot.visual && mediaRoot.individuallySelected
        imageEditor: mediaRoot
        direction: 1
        fillColor: altEditor.palette.base
        strokeColor: altEditor.palette.highlight
        x: Math.min(mediaRoot.width - width,
                    sourceImage.x + sourceImage.width - width / 2)
        y: sourceImage.y + sourceImage.height - height / 2
    }

    Row {
        id: imageActions
        objectName: "mediaVisualActions-" + mediaRoot.block.index
        visible: mediaRoot.visual && mediaRoot.individuallySelected
        spacing: 3
        height: mediaRoot.editorView.touchMode ? 36 : 28
        y: sourceImage.y + sourceImage.height + mediaRoot.actionGap
        x: Math.max(0, Math.min(mediaRoot.width - width,
                                sourceImage.x + (sourceImage.width - width) / 2))

        Repeater {
            model: ["left", "center", "right"]
            delegate: ToolButton {
                id: alignmentButton
                required property string modelData
                width: imageActions.height
                height: imageActions.height
                padding: 4
                checkable: true
                checked: mediaRoot.normalizedAlignment === modelData
                display: AbstractButton.IconOnly
                ToolTip.visible: hovered
                ToolTip.text: modelData === "left" ? qsTr("Align left")
                              : modelData === "right" ? qsTr("Align right")
                              : qsTr("Align center")
                contentItem: ImageAlignmentGlyph {
                    alignment: modelData
                    tint: alignmentButton.palette.buttonText
                }
                onClicked: mediaRoot.setAlignment(modelData)
            }
        }

        ToolButton {
            id: resetImageButton
            objectName: "mediaResetPresentationButton-" + mediaRoot.block.index
            width: imageActions.height
            height: imageActions.height
            padding: 4
            display: AbstractButton.IconOnly
            enabled: mediaRoot.block.mediaDisplayWidth > 0 || mediaRoot.normalizedAlignment !== "center"
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Reset media size and alignment")
            contentItem: Shared.ThemedIconImpl {
                objectName: "mediaResetPresentationGlyph-" + mediaRoot.block.index
                themeName: "__bundled__"
                fallbackName: "view-refresh-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: String(resetImageButton.palette.buttonText)
                pixelSize: 20
            }
            onClicked: mediaRoot.resetPresentation()
        }
    }

    Rectangle {
        id: transcriptPanel
        x: 0
        y: mediaRoot.baseContentHeight + 6
        width: mediaRoot.width
        visible: mediaRoot.transcriptExpanded && mediaRoot.block.mediaTranscript.length > 0
        implicitHeight: visible ? transcriptText.implicitHeight + 16 : 0
        radius: 5
        color: mediaRoot.editorView.documentCardColor
        border.width: 1
        border.color: mediaRoot.editorView.documentCardBorderColor

        TextArea {
            id: transcriptText
            anchors.fill: parent
            anchors.margins: 4
            readOnly: true
            text: mediaRoot.block.mediaTranscript
            wrapMode: TextEdit.Wrap
            selectByMouse: !mediaRoot.editorView.touchMode
            background: null
            color: mediaRoot.editorView.documentTextColor
            Accessible.name: qsTr("Media transcript")
        }
    }

    Connections {
        target: mediaRoot.block
        function onMediaTranscriptChanged() {
            if (mediaRoot.block.mediaTranscript.length > 0)
                mediaRoot.transcriptExpanded = true
        }
    }

    Menu {
        id: mediaContextMenu
        MenuItem {
            text: qsTr("Save Image As…")
            visible: mediaRoot.block.mediaType.startsWith("image/")
                     && mediaRoot.editorView.platformBackend !== null
            height: visible ? implicitHeight : 0
            enabled: visible && mediaRoot.block.url.startsWith("anykeep-media:/")
            onTriggered: mediaRoot.editorView.platformBackend.saveImageAs(mediaRoot.block.url)
        }
        MenuSeparator { visible: mediaRoot.visual }
        MenuItem {
            text: qsTr("Align Left")
            visible: mediaRoot.visual
            height: visible ? implicitHeight : 0
            onTriggered: mediaRoot.setAlignment("left")
        }
        MenuItem {
            text: qsTr("Align Center")
            visible: mediaRoot.visual
            height: visible ? implicitHeight : 0
            onTriggered: mediaRoot.setAlignment("center")
        }
        MenuItem {
            text: qsTr("Align Right")
            visible: mediaRoot.visual
            height: visible ? implicitHeight : 0
            onTriggered: mediaRoot.setAlignment("right")
        }
        MenuItem {
            text: qsTr("Reset Size and Alignment")
            visible: mediaRoot.visual
            height: visible ? implicitHeight : 0
            enabled: mediaRoot.block.mediaDisplayWidth > 0 || mediaRoot.normalizedAlignment !== "center"
            onTriggered: mediaRoot.resetPresentation()
        }
        MenuSeparator { visible: mediaRoot.canTranscribe }
        MenuItem {
            text: mediaRoot.block.mediaTranscript.length > 0 ? qsTr("Show transcript") : qsTr("Transcribe audio")
            visible: mediaRoot.canTranscribe
            height: visible ? implicitHeight : 0
            enabled: !mediaRoot.transcriptionBlocked && !mediaRoot.transcribing
            onTriggered: {
                if (mediaRoot.block.mediaTranscript.length > 0)
                    mediaRoot.transcriptExpanded = true
                else
                    mediaRoot.requestTranscription()
            }
        }
        MenuItem {
            text: qsTr("Transcribe again")
            visible: mediaRoot.canTranscribe && mediaRoot.block.mediaTranscript.length > 0
            height: visible ? implicitHeight : 0
            enabled: !mediaRoot.transcriptionBlocked && !mediaRoot.transcribing
            onTriggered: mediaRoot.requestTranscription()
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Remove Media")
            onTriggered: mediaRoot.editorView.removeMediaBlock(mediaRoot.block.index, true)
        }
    }

}
