import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../shared" as Shared
import ".." as EditorComponents

// Shared audio/video transport. Audio adds a title row; video adds window
// controls. Each action and playback binding has one implementation.
Rectangle {
    id: transport
    required property var mediaBlock
    property bool embedded: false
    property bool touchMode: mediaBlock.editorView.touchMode
    readonly property real buttonExtent: touchMode ? 48 : 32
    readonly property real contentInset: touchMode ? 6 : 4
    readonly property real rowSpacing: touchMode ? 8 : 2
    readonly property real controlPadding: 6
    implicitHeight: contentColumn.implicitHeight + 2 * contentInset
    readonly property string timeLabel:
        mediaBlock.formatTime(mediaBlock.current ? mediaBlock.playback.position : 0)
        + " / " + mediaBlock.formatTime(mediaBlock.knownDuration)
    objectName: "mediaCard-" + mediaBlock.block.index
    color: mediaBlock.editorView.documentCardColor
    radius: embedded ? 0 : 5
    clip: true

    TapHandler {
        acceptedButtons: Qt.LeftButton
        onPressedChanged: {
            const local = inlineTitle.mapFromItem(transport, point.position.x, point.position.y)
            if (pressed && (!mediaBlock.audio || !inlineTitle.contains(local)))
                mediaBlock.selectAndFocus()
        }
    }

    ColumnLayout {
        id: contentColumn
        objectName: "mediaTransportLayout-" + mediaBlock.block.index
        anchors.fill: parent
        anchors.margins: transport.contentInset
        spacing: transport.rowSpacing

        RowLayout {
            objectName: "mediaAudioHeader-" + mediaBlock.block.index
            visible: mediaBlock.audio
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            // Match the padded content edges of the outer transport buttons,
            // while those buttons retain their full touch targets.
            Layout.leftMargin: mediaPlaybackButton.leftPadding
            Layout.rightMargin: mediaActionsButton.rightPadding
            spacing: 4
            Loader {
                id: inlineTitle
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignVCenter
                active: mediaBlock.audio
                sourceComponent: Component {
                    MediaTitleField {
                        mediaBlock: transport.mediaBlock
                        editable: mediaBlock.canEditTitle
                    }
                }
            }
            Label {
                id: audioTime
                objectName: "mediaAudioTime-" + mediaBlock.block.index
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                color: mediaBlock.editorView.documentSecondaryTextColor
                text: transport.timeLabel
            }
        }

        RowLayout {
            id: controlsRow
            objectName: "mediaTransportRow-" + mediaBlock.block.index
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.alignment: Qt.AlignVCenter
            Layout.minimumHeight: transport.buttonExtent
            spacing: 4

            ToolButton {
                id: mediaPlaybackButton
                objectName: "mediaPlaybackButton-" + mediaBlock.block.index
                Layout.preferredWidth: transport.buttonExtent
                Layout.preferredHeight: transport.buttonExtent
                enabled: mediaBlock.playback && mediaBlock.playback.available
                padding: transport.controlPadding
                focusPolicy: Qt.TabFocus
                Keys.onPressed: function(event) { mediaBlock.handleNavigationKey(event) }
                display: AbstractButton.IconOnly
                contentItem: Item {
                    implicitWidth: 20
                    implicitHeight: 20
                    Shared.ThemedIconImpl {
                        objectName: "mediaPlaybackGlyph-" + mediaBlock.block.index
                        anchors.centerIn: parent
                        visible: !mediaBlock.loading
                        themeName: "__bundled__"
                        fallbackName: mediaBlock.playing ? "media-playback-pause-symbolic.svg"
                                                        : "media-playback-start-symbolic.svg"
                        recolorFallback: true
                        fallbackTintMode: String(mediaPlaybackButton.palette.buttonText)
                        pixelSize: 20
                    }
                    BusyIndicator {
                        objectName: "mediaPlaybackBusyIndicator-" + mediaBlock.block.index
                        anchors.centerIn: parent
                        width: 22
                        height: 22
                        visible: mediaBlock.loading
                        running: visible
                    }
                }
                Accessible.name: mediaBlock.loading ? qsTr("Loading media")
                                                  : (mediaBlock.playing ? qsTr("Pause media") : qsTr("Play media"))
                onClicked: {
                    mediaBlock.selectAndFocus()
                    mediaBlock.playback.toggle(mediaBlock.block.url)
                }
            }

            Slider {
                id: seekSlider
                objectName: "mediaSeekSlider-" + mediaBlock.block.index
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.minimumHeight: transport.buttonExtent
                from: 0
                to: Math.max(1, mediaBlock.knownDuration)
                value: mediaBlock.current ? mediaBlock.playback.position : 0
                enabled: mediaBlock.playback && mediaBlock.knownDuration > 0
                focusPolicy: Qt.TabFocus
                onPressedChanged: if (pressed) mediaBlock.selectAndFocus()
                Keys.onPressed: function(event) { mediaBlock.handleNavigationKey(event) }
                onMoved: mediaBlock.playback.seek(mediaBlock.block.url, value)
            }

            Label {
                objectName: "mediaVideoTime-" + mediaBlock.block.index
                visible: mediaBlock.video
                text: transport.timeLabel
                color: mediaBlock.editorView.documentSecondaryTextColor
            }

            EditorComponents.MediaSyncIndicator {
                objectName: "mediaAudioSync-" + mediaBlock.block.index
                visible: mediaBlock.audio && Boolean(mediaBlock.syncStatus.valid)
                Layout.preferredWidth: visible ? transport.buttonExtent : 0
                Layout.preferredHeight: transport.buttonExtent
                progress: mediaBlock.syncStatus.progress || 0
                syncState: mediaBlock.syncStatus.state || 0
                errorString: mediaBlock.syncStatus.error || ""
            }

            ToolButton {
                objectName: "mediaTranscriptionButton-" + mediaBlock.block.index
                visible: mediaBlock.audio && mediaBlock.canTranscribe
                Layout.preferredWidth: Math.max(transport.buttonExtent, contentItem.implicitWidth + leftPadding + rightPadding)
                Layout.preferredHeight: transport.buttonExtent
                padding: transport.controlPadding
                enabled: !mediaBlock.transcriptionBlocked && !mediaBlock.transcribing
                focusPolicy: Qt.TabFocus
                Keys.onPressed: function(event) { mediaBlock.handleNavigationKey(event) }
                text: mediaBlock.transcribing ? "…" : "STT"
                Accessible.name: mediaBlock.block.mediaTranscript.length > 0
                                 ? qsTr("Show transcript") : qsTr("Transcribe audio")
                onClicked: {
                    mediaBlock.selectAndFocus()
                    if (mediaBlock.block.mediaTranscript.length > 0)
                        mediaBlock.transcriptExpanded = !mediaBlock.transcriptExpanded
                    else
                        mediaBlock.requestTranscription()
                }
            }

            ToolButton {
                id: fullScreenMediaButton
                objectName: "mediaFullscreenButton-" + mediaBlock.block.index
                visible: mediaBlock.video
                Layout.preferredWidth: transport.buttonExtent
                Layout.preferredHeight: transport.buttonExtent
                enabled: mediaBlock.playback && mediaBlock.playback.available
                padding: transport.controlPadding
                focusPolicy: Qt.TabFocus
                Keys.onPressed: function(event) { mediaBlock.handleNavigationKey(event) }
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "mediaFullscreenGlyph-" + mediaBlock.block.index
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
                    mediaBlock.selectAndFocus()
                    mediaBlock.openVideoFullScreen()
                }
            }
            ToolButton {
                id: detachVideoButton
                objectName: "mediaSeparateWindowButton-" + mediaBlock.block.index
                visible: mediaBlock.video && mediaBlock.playback && typeof mediaBlock.playback.openWindow === "function"
                enabled: visible && mediaBlock.playback.available
                Layout.preferredWidth: transport.buttonExtent
                Layout.preferredHeight: transport.buttonExtent
                focusPolicy: Qt.TabFocus
                Keys.onPressed: function(event) { mediaBlock.handleNavigationKey(event) }
                padding: transport.controlPadding
                contentItem: Shared.ThemedIconImpl {
                    themeName: "__bundled__"
                    fallbackName: "open-external-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(detachVideoButton.palette.buttonText)
                    pixelSize: 20
                }
                Accessible.name: qsTr("Open video in a separate window")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    mediaBlock.selectAndFocus()
                    mediaBlock.openVideoWindow()
                }
            }

            ToolButton {
                id: mediaActionsButton
                objectName: "mediaActionsButton-" + mediaBlock.block.index
                Layout.preferredWidth: transport.buttonExtent
                Layout.preferredHeight: transport.buttonExtent
                padding: transport.controlPadding
                focusPolicy: Qt.TabFocus
                Keys.onPressed: function(event) { mediaBlock.handleNavigationKey(event) }
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "mediaActionsGlyph-" + mediaBlock.block.index
                    themeName: "__bundled__"
                    fallbackName: "view-more-vertical-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(mediaActionsButton.palette.buttonText)
                    pixelSize: 20
                }
                Accessible.name: qsTr("Media actions")
                onClicked: {
                    mediaBlock.selectAndFocus()
                    mediaBlock.openMediaContextMenu()
                }
            }
        }
    }
}
