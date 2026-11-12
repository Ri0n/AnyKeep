import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../reorder" as Reorder
import "../../shared" as Shared
import ".." as EditorComponents

FocusScope {
    id: mediaRoot
    required property var editorView
    required property var reorderController
    objectName: "mediaBlockEditor-" + block.index
    required property var block
    property var playback: mediaRoot.editorView.editorBackend ? mediaRoot.editorView.editorBackend.mediaPlayback : null
    property var mediaSync: mediaRoot.editorView.editorBackend ? mediaRoot.editorView.editorBackend.mediaSync : null
    readonly property var syncStatus: {
        const generation = mediaRoot.mediaSync ? mediaRoot.mediaSync.revision : 0
        return mediaRoot.mediaSync ? mediaRoot.mediaSync.statusForUri(mediaRoot.block.url)
                                   : ({ valid: false, progress: 0, state: 0 })
    }
    property var transcription: mediaRoot.editorView.audioTranscriptionController
    property bool transcriptExpanded: false
    property bool titleEditing: false
    readonly property bool canEditTitle: true
    readonly property var platform: editorView.platformBackend
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
    readonly property color selectionColor: altEditor.palette.highlight
    property real transientWidth: -1
    property real transientX: -1
    property real resizeStartWidth: 0
    property real resizeStartX: 0
    property int resizeDirection: 1
    readonly property string normalizedAlignment:
        block.mediaAlignment === "left" || block.mediaAlignment === "right"
        ? block.mediaAlignment : "center"
    readonly property real naturalWidth:
        !video && sourceImage.implicitWidth > 0 ? sourceImage.implicitWidth : Math.min(width, 480)
    readonly property real requestedWidth:
        transientWidth >= 0 ? transientWidth
                            : (block.mediaDisplayWidth > 0 ? block.mediaDisplayWidth : naturalWidth)
    readonly property real displayWidth: Math.max(1, Math.min(width, requestedWidth))
    readonly property var visualItem: video ? videoCard : sourceImage
    readonly property real transportHeight: mediaRoot.editorView.touchMode ? 48 : 40
    // Descriptor dimensions are authoritative. An optional poster may supply
    // the ratio only if the source video has no valid dimensions.
    readonly property real visualAspect:
        video && block.mediaWidth > 0 && block.mediaHeight > 0
        ? block.mediaHeight / block.mediaWidth
        : (video && videoCard.posterImplicitWidth > 0 && videoCard.posterImplicitHeight > 0
           ? videoCard.posterImplicitHeight / videoCard.posterImplicitWidth
           : (sourceImage.implicitWidth > 0 && sourceImage.implicitHeight > 0
              ? sourceImage.implicitHeight / sourceImage.implicitWidth
              : (block.mediaWidth > 0 && block.mediaHeight > 0
                 ? block.mediaHeight / block.mediaWidth : (video ? 9 / 16 : 0.75))))
    // The video frame preserves its intrinsic aspect ratio; controls take up
    // their own height inside the single card instead of hiding its last 40 px.
    readonly property real displayHeight: video ? Math.max(40, displayWidth * visualAspect) + transportHeight + 2
        : visual ? Math.max(40, displayWidth * visualAspect) : 58
    readonly property real imageY: individuallySelected && !timed ? altEditor.implicitHeight + 6 : 0
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
    signal finishTitleEditing()

    function selectAndFocus() {
        finishTitleEditing()
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
        resizeStartX = visualItem.x
        transientWidth = displayWidth
        transientX = visualItem.x
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

    function openMediaContextMenu() {
        selectAndFocus()
        mediaContextMenu.rebuild()
        mediaContextMenu.popup()
    }

    function openVideoFullScreen() {
        if (video)
            if (videoCard.item)
                videoCard.item.openFullScreen()
    }

    function openVideoWindow() {
        if (playback && typeof playback.openWindow === "function")
            playback.openWindow(block.url, block.alt, String(block.previewUrl), Qt.resolvedUrl("MediaPlayerWindow.qml"))
    }

    function setTitle(value) {
        mediaRoot.editorView.blockModel.setMediaTitle(block.index, value)
    }

    function handleNavigationKey(event) {
        if (titleEditing || event.modifiers)
            return false
        if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
            editorView.navigateBlock(block.index, event.key === Qt.Key_Up ? -1 : 1, !event.isAutoRepeat)
            event.accepted = true
            return true
        }
        if (visual && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
            const alignments = ["left", "center", "right"]
            const next = Math.max(0, Math.min(2, alignments.indexOf(normalizedAlignment)
                                            + (event.key === Qt.Key_Left ? -1 : 1)))
            setAlignment(alignments[next])
            event.accepted = true
            return true
        }
        return false
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
        if (altEditor.activeFocus || titleEditing)
            return
        if (handleNavigationKey(event))
            return
        if (timed && !event.modifiers && (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
            if (playback)
                playback.toggle(block.url)
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
        visible: mediaRoot.individuallySelected && !mediaRoot.timed
        opacity: visible ? 1 : 0
        width: Math.min(mediaRoot.width, Math.max(180, visualItem.width))
        x: Math.max(0, Math.min(mediaRoot.width - width,
                                visualItem.x + (visualItem.width - width) / 2))
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
        visible: mediaRoot.visual && !mediaRoot.video
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
        cache: true
        Behavior on x {
            enabled: mediaRoot.transientWidth < 0
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }

    }

    Loader {
        id: videoCard
        active: mediaRoot.video
        sourceComponent: Component {
            MediaVideoCard { mediaBlock: mediaRoot }
        }
        x: mediaRoot.displayX
        y: mediaRoot.imageY
        width: mediaRoot.displayWidth
        height: mediaRoot.displayHeight

        readonly property real posterImplicitWidth: item ? item.posterImplicitWidth : 0
        readonly property real posterImplicitHeight: item ? item.posterImplicitHeight : 0

        Behavior on x {
            enabled: mediaRoot.transientWidth < 0
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
    }

    // MediaBlock owns editor-specific gestures for *both* images and videos.
    // The hit region stops at the video transport: clicking its controls
    // never starts a document drag or changes the structural selection.
    Item {
        id: visualHitLayer
        objectName: "mediaVisualHitLayer-" + mediaRoot.block.index
        visible: mediaRoot.visual
        x: visualItem.x + (mediaRoot.video ? 1 : 0)
        readonly property real titleInset: mediaRoot.video && videoCard.item ? videoCard.item.titleAreaHeight : 0
        y: visualItem.y + (mediaRoot.video ? 1 : 0) + titleInset
        width: Math.max(0, visualItem.width - (mediaRoot.video ? 2 : 0))
        height: Math.max(0, visualItem.height - (mediaRoot.video ? mediaRoot.transportHeight + 2 : 0) - titleInset)
        z: 2

        HoverHandler { id: visualDescriptionHover }
        ToolTip.visible: visualDescriptionHover.hovered && mediaRoot.block.alt.length > 0
                         && !mediaRoot.selected
        ToolTip.text: mediaRoot.block.alt

        Reorder.ReorderDragHandle {
            anchors.fill: parent
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

    // Horizontal margins remain part of the editor's document selection area.
    // A drag from either side of a visual block selects the text boundary.
    MouseArea {
        id: imageLeftMarginSelectionArea
        x: 0
        y: visualItem.y
        width: Math.max(0, visualItem.x)
        height: visualItem.height
        z: 1
        enabled: mediaRoot.visual && !mediaRoot.editorView.touchMode
                 && !mediaRoot.reorderController.dragging && width > 0
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
        x: visualItem.x + visualItem.width
        y: visualItem.y
        width: Math.max(0, mediaRoot.width - x)
        height: visualItem.height
        z: 1
        enabled: mediaRoot.visual && !mediaRoot.editorView.touchMode
                 && !mediaRoot.reorderController.dragging && width > 0
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        preventStealing: true
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) { mediaRoot.beginMarginSelection(imageRightMarginSelectionArea, mouse) }
        onPositionChanged: function(mouse) { mediaRoot.updateMarginSelection(imageRightMarginSelectionArea, mouse) }
        onReleased: function(mouse) { mediaRoot.finishMarginSelection(mouse) }
        onCanceled: mediaRoot.editorView.cancelBlankAreaSelection()
    }

    Loader {
        id: audioControls
        active: mediaRoot.audio
        sourceComponent: Component {
            MediaTransportControls { mediaBlock: mediaRoot }
        }
        x: 0
        y: mediaRoot.imageY
        width: mediaRoot.width
        height: mediaRoot.displayHeight
    }

    Rectangle {
        id: selectionOutline
        objectName: "mediaSelectionOutline-" + mediaRoot.block.index
        x: (mediaRoot.visual ? visualItem.x : audioControls.x) - 2
        y: (mediaRoot.visual ? visualItem.y : audioControls.y) - 2
        width: (mediaRoot.visual ? visualItem.width : audioControls.width) + 4
        height: (mediaRoot.visual ? visualItem.height : audioControls.height) + 4
        visible: mediaRoot.selected && !mediaRoot.video
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
        x: Math.max(0, visualItem.x - width / 2)
        y: visualItem.y + visualItem.height - height / 2
    }

    ImageResizeHandle {
        visible: mediaRoot.visual && mediaRoot.individuallySelected
        imageEditor: mediaRoot
        direction: 1
        fillColor: altEditor.palette.base
        strokeColor: altEditor.palette.highlight
        x: Math.min(mediaRoot.width - width,
                    visualItem.x + visualItem.width - width / 2)
        y: visualItem.y + visualItem.height - height / 2
    }

    Row {
        id: imageActions
        objectName: "mediaVisualActions-" + mediaRoot.block.index
        visible: mediaRoot.visual && mediaRoot.individuallySelected
        spacing: 3
        height: mediaRoot.editorView.touchMode ? 36 : 28
        y: visualItem.y + visualItem.height + mediaRoot.actionGap
        x: Math.max(0, Math.min(mediaRoot.width - width,
                                visualItem.x + (visualItem.width - width) / 2))

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

        EditorComponents.MediaSyncIndicator {
            objectName: "mediaVisualSync-" + mediaRoot.block.index
            visible: Boolean(mediaRoot.syncStatus.valid)
            width: visible ? imageActions.height : 0
            height: imageActions.height
            progress: mediaRoot.syncStatus.progress || 0
            syncState: mediaRoot.syncStatus.state || 0
            errorString: mediaRoot.syncStatus.error || ""
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

    function contextMenuEntries() {
        const entries = []
        function section(items) {
            if (items.length === 0)
                return
            if (entries.length > 0)
                entries.push({ separator: true })
            for (const item of items)
                entries.push(item)
        }
        const storage = []
        if (platform && block.url.startsWith("anykeep-media:/")) {
            // Query on every opening: another attachment/window can change
            // where the same content is stored without changing this block.
            const managed = typeof platform.isAttachmentInMediaStore === "function"
                            && platform.isAttachmentInMediaStore(block.url)
            const keptInPlace = typeof platform.isAttachmentKeptInPlace === "function"
                                && platform.isAttachmentKeptInPlace(block.url)
            const hasLocalFile = typeof platform.hasAttachmentLocalFile === "function"
                                 && platform.hasAttachmentLocalFile(block.url)
            if (managed && typeof platform.moveAttachmentOutOfStore === "function")
                storage.push({ text: qsTr("Move out of encrypted storage…"), command: "move-out" })
            else if (keptInPlace && typeof platform.copyAttachmentIntoStore === "function")
                storage.push({ text: qsTr("Copy into encrypted storage"), command: "copy-in" })
            // One export action for every media source. Unlike the removed
            // saveImageAs path, this supports verified remote XMPP ranges as
            // well as local managed and external files.
            if (typeof platform.saveAttachmentAs === "function")
                storage.push({ text: qsTr("Save To…"), command: "save" })
            if (!managed && hasLocalFile && typeof platform.showAttachmentInFolder === "function")
                storage.push({ text: qsTr("Show in Folder"), command: "show" })
        }
        section(storage)
        if (visual) {
            section([{ text: qsTr("Align Left"), command: "left" },
                     { text: qsTr("Align Center"), command: "center" },
                     { text: qsTr("Align Right"), command: "right" },
                     { text: qsTr("Reset Size and Alignment"), command: "reset",
                       enabled: block.mediaDisplayWidth > 0 || normalizedAlignment !== "center" }])
        }
        if (canTranscribe) {
            const actions = [{ text: block.mediaTranscript.length > 0 ? qsTr("Show transcript") : qsTr("Transcribe audio"),
                               command: "transcript", enabled: !transcriptionBlocked && !transcribing }]
            if (block.mediaTranscript.length > 0)
                actions.push({ text: qsTr("Transcribe again"), command: "transcribe",
                               enabled: !transcriptionBlocked && !transcribing })
            section(actions)
        }
        section([{ text: qsTr("Remove Media"), command: "remove" }])
        return entries
    }
    function runMenuCommand(command) {
        if (command === "move-out") platform.moveAttachmentOutOfStore(block.url)
        else if (command === "copy-in") platform.copyAttachmentIntoStore(block.url)
        else if (command === "save") platform.saveAttachmentAs(block.url)
        else if (command === "show") platform.showAttachmentInFolder(block.url)
        else if (command === "left" || command === "right" || command === "center") setAlignment(command)
        else if (command === "reset") resetPresentation()
        else if (command === "transcript" && block.mediaTranscript.length > 0) transcriptExpanded = true
        else if (command === "transcript" || command === "transcribe") requestTranscription()
        else if (command === "remove") editorView.removeMediaBlock(block.index, true)
    }
    Menu {
        id: mediaContextMenu
        objectName: "mediaContextMenu-" + mediaRoot.block.index
        function rebuild() {
            while (count > 0) {
                const old = takeItem(0)
                old.destroy()
            }
            for (const entry of mediaRoot.contextMenuEntries()) {
                const item = entry.separator ? separatorFactory.createObject(mediaRoot)
                                             : menuItemFactory.createObject(mediaRoot, {
                                                   text: entry.text, command: entry.command,
                                                   enabled: entry.enabled === undefined ? true : entry.enabled })
                addItem(item)
            }
        }
    }
    Component { id: separatorFactory; MenuSeparator {} }
    Component {
        id: menuItemFactory
        MenuItem {
            property string command
            onTriggered: mediaRoot.runMenuCommand(command)
        }
    }
}
