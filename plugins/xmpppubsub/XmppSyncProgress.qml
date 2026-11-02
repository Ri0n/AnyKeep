pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

GroupBox {
    id: root

    property string summary: ""
    property var events: []
    property bool busy: false
    property int completed: -1
    property int total: -1
    property bool expanded: false
    property real logHeight: 150
    property int elapsedSeconds: 0

    title: qsTr("Synchronization")
    visible: busy || events.length > 0
    onBusyChanged: {
        if (busy)
            elapsedSeconds = 0;
    }

    Timer {
        interval: 1000
        repeat: true
        running: root.busy
        onTriggered: root.elapsedSeconds += 1
    }

    contentItem: ColumnLayout {
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            BusyIndicator {
                running: root.busy
                visible: running
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
            }

            Label {
                objectName: "xmppSyncSummary"
                Layout.fillWidth: true
                text: root.summary
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }

            ToolButton {
                objectName: "xmppSyncLogToggle"
                checkable: true
                checked: root.expanded
                text: checked ? qsTr("Hide log") : qsTr("Show log")
                onClicked: root.expanded = checked
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.total >= 0 || root.busy || root.elapsedSeconds > 0

            Label {
                visible: root.total >= 0
                text: qsTr("%1 / %2 notes").arg(Math.max(0, root.completed)).arg(root.total)
                color: palette.placeholderText
            }

            Item { Layout.fillWidth: true }

            Label {
                objectName: "xmppSyncElapsed"
                text: qsTr("Elapsed: %1 s").arg(root.elapsedSeconds)
                color: palette.placeholderText
            }
        }

        ProgressBar {
            objectName: "xmppSyncCounter"
            Layout.fillWidth: true
            visible: root.total > 0
            from: 0
            to: 1
            value: root.total > 0 ? Math.max(0, root.completed) / root.total : 0
        }

        ScrollView {
            id: logScroll
            objectName: "xmppSyncLogViewport"

            Layout.fillWidth: true
            Layout.minimumHeight: 0
            Layout.preferredHeight: root.logHeight
            Layout.maximumHeight: root.logHeight
            visible: root.expanded && root.events.length > 0
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            TextArea {
                objectName: "xmppSyncEvents"
                width: logScroll.availableWidth
                text: root.events.join("\n")
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                font.pixelSize: 12
                color: palette.text
                onTextChanged: cursorPosition = length
            }
        }
    }
}
