import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: root

    required property var blockEditor
    property bool expanded: false
    property alias query: searchField.text
    signal closeRequested()

    function open() {
        expanded = true
        Qt.callLater(function() {
            searchField.forceActiveFocus()
            searchField.selectAll()
        })
    }

    function close() {
        expanded = false
        blockEditor.resetFind()
        closeRequested()
    }

    visible: implicitHeight > 0
    enabled: expanded
    opacity: expanded ? 1 : 0
    implicitHeight: expanded ? contentRow.implicitHeight + topPadding + bottomPadding : 0
    padding: expanded ? 6 : 0

    Behavior on implicitHeight { NumberAnimation { duration: 120 } }
    Behavior on opacity { NumberAnimation { duration: 100 } }

    RowLayout {
        id: contentRow
        anchors.fill: parent
        spacing: 4

        TextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: qsTr("Find in note")
            selectByMouse: true
            onTextEdited: root.blockEditor.resetFind()
            onAccepted: root.blockEditor.findNext(text, false)
            Keys.onEscapePressed: root.close()
        }

        ToolButton {
            text: "↑"
            enabled: searchField.text.length > 0
            Accessible.name: qsTr("Previous match")
            onClicked: root.blockEditor.findNext(searchField.text, true)
        }

        ToolButton {
            text: "↓"
            enabled: searchField.text.length > 0
            Accessible.name: qsTr("Next match")
            onClicked: root.blockEditor.findNext(searchField.text, false)
        }

        ToolButton {
            text: "×"
            Accessible.name: qsTr("Close find")
            onClicked: root.close()
        }
    }
}
