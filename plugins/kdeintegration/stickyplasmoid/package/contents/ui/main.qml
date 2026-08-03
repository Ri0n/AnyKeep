/*
    SPDX-License-Identifier: GPL-3.0-only
*/

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import plasma.applet.com.github.ri0n.anykeep.sticky 1.0 as AnyKeep

PlasmoidItem {
    id: root

    preferredRepresentation: fullRepresentation
    Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground
    Plasmoid.icon: "anykeep-symbolic"
    Plasmoid.status: PlasmaCore.Types.ActiveStatus

    toolTipMainText: stickyNote.title || qsTr("AnyKeep Sticky Note")
    toolTipSubText: stickyNote.available
        ? qsTr("Double-click to edit")
        : qsTr("AnyKeep is not running")

    AnyKeep.StickyNoteModel {
        id: stickyNote
        presentationId: String(Plasmoid.id)
    }

    fullRepresentation: Rectangle {
        color: "#fff2a8"
        border.color: "#665a2b"
        border.width: 1
        radius: Kirigami.Units.smallSpacing

        Layout.minimumWidth: Kirigami.Units.gridUnit * 11
        Layout.minimumHeight: Kirigami.Units.gridUnit * 7
        Layout.preferredWidth: Kirigami.Units.gridUnit * 16
        Layout.preferredHeight: Kirigami.Units.gridUnit * 11

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onDoubleClicked: stickyNote.open()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing

            Text {
                Layout.fillWidth: true
                color: "#2f2a18"
                elide: Text.ElideRight
                font.family: Kirigami.Theme.defaultFont.family
                font.pointSize: Kirigami.Theme.defaultFont.pointSize * 1.1
                font.weight: Font.Bold
                text: stickyNote.title || qsTr("[No Title]")
            }

            Text {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#3d361f"
                elide: Text.ElideRight
                font: Kirigami.Theme.defaultFont
                text: stickyNote.available
                    ? stickyNote.body
                    : qsTr("AnyKeep is not running")
                textFormat: Text.PlainText
                verticalAlignment: Text.AlignTop
                wrapMode: Text.Wrap
            }
        }
    }

    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: qsTr("Edit Note")
            icon.name: "document-edit-symbolic"
            enabled: stickyNote.available && stickyNote.stickyId.length > 0
            onTriggered: stickyNote.open()
        },
        PlasmaCore.Action {
            text: qsTr("Unpin")
            icon.name: "window-close-symbolic"
            enabled: stickyNote.available && stickyNote.stickyId.length > 0
            onTriggered: stickyNote.unpin()
        }
    ]
}
