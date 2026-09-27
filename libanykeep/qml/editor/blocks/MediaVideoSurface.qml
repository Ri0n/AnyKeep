import QtQuick
import QtQuick.Controls
import QtMultimedia

Item {
    id: root
    property var playback: null
    property var attachedPlayback: null

    function attach() {
        if (attachedPlayback === playback)
            return
        if (attachedPlayback)
            attachedPlayback.detachVideoOutput(videoOutput)
        attachedPlayback = playback
        if (attachedPlayback)
            attachedPlayback.attachVideoOutput(videoOutput)
    }

    function openFullScreen() { fullScreenPopup.open() }
    function closeFullScreen() { fullScreenPopup.close() }

    onPlaybackChanged: attach()
    Component.onCompleted: attach()
    Component.onDestruction: {
        if (attachedPlayback)
            attachedPlayback.detachVideoOutput(videoOutput)
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
        z: 0
    }

    Popup {
        id: fullScreenPopup
        parent: Overlay.overlay
        modal: true
        focus: true
        padding: 0
        x: 0
        y: 0
        width: Overlay.overlay ? Overlay.overlay.width : 0
        height: Overlay.overlay ? Overlay.overlay.height : 0
        closePolicy: Popup.CloseOnEscape

        background: Rectangle { color: "black" }

        onOpened: videoOutput.parent = contentItem
        onClosed: videoOutput.parent = root

        ToolButton {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 16
            text: "×"
            z: 2
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Exit full screen")
            Accessible.name: qsTr("Exit full screen video")
            onClicked: fullScreenPopup.close()
        }
    }
}
