import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../shared" as Shared
import ".." as EditorComponents

// Transport controls are shared by the audio-only card and the video card.
// Neither the slider nor the buttons own media playback state or editor history.
Rectangle {
    id: transport
    required property var mediaBlock
    property bool embedded: false
    objectName: "mediaCard-" + mediaBlock.block.index
    color: mediaBlock.editorView.documentCardColor
    radius: embedded ? 0 : 5

    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 6
        ToolButton {
            id: mediaPlaybackButton
            objectName: "mediaPlaybackButton-" + mediaBlock.block.index
            Layout.preferredWidth: parent.height - 2
            Layout.preferredHeight: Layout.preferredWidth
            enabled: mediaBlock.playback && mediaBlock.playback.available
            padding: 6
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
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Label {
                objectName: "mediaLabel-" + mediaBlock.block.index
                Layout.fillWidth: true
                visible: !mediaBlock.visual
                text: mediaBlock.block.alt.length > 0 ? mediaBlock.block.alt : qsTr("Audio recording")
                elide: Text.ElideMiddle
                font.bold: mediaBlock.selected
                color: mediaBlock.editorView.documentTextColor
            }
            Slider {
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, mediaBlock.knownDuration)
                value: mediaBlock.current ? mediaBlock.playback.position : 0
                enabled: mediaBlock.playback && mediaBlock.knownDuration > 0
                onMoved: mediaBlock.playback.seek(mediaBlock.block.url, value)
            }
        }
        Label {
            text: mediaBlock.formatTime(mediaBlock.current ? mediaBlock.playback.position : 0)
                  + " / " + mediaBlock.formatTime(mediaBlock.knownDuration)
            color: mediaBlock.editorView.documentSecondaryTextColor
        }
        EditorComponents.MediaSyncIndicator {
            objectName: "mediaAudioSync-" + mediaBlock.block.index
            visible: mediaBlock.audio && Boolean(mediaBlock.syncStatus.valid)
            Layout.preferredWidth: visible ? parent.height - 2 : 0
            Layout.preferredHeight: Layout.preferredWidth
            progress: mediaBlock.syncStatus.progress || 0
            syncState: mediaBlock.syncStatus.state || 0
            errorString: mediaBlock.syncStatus.error || ""
        }
        ToolButton {
            visible: mediaBlock.canTranscribe
            Layout.preferredWidth: parent.height - 2
            Layout.preferredHeight: Layout.preferredWidth
            enabled: !mediaBlock.transcriptionBlocked && !mediaBlock.transcribing
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
            Layout.preferredWidth: parent.height - 2
            Layout.preferredHeight: Layout.preferredWidth
            enabled: mediaBlock.playback && mediaBlock.playback.available
            padding: 6
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
            id: mediaActionsButton
            objectName: "mediaActionsButton-" + mediaBlock.block.index
            Layout.preferredWidth: parent.height - 2
            Layout.preferredHeight: Layout.preferredWidth
            padding: 6
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
