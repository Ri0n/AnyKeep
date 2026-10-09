/*
    SPDX-License-Identifier: GPL-3.0-only
*/

pragma ComponentBehavior: Bound

import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasmoid

MouseArea {
    id: root

    required property PlasmoidItem plasmoidItem
    required property var notesModel

    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
    hoverEnabled: true

    onClicked: mouse => {
        if (mouse.button === Qt.MiddleButton) {
            root.notesModel.createNote();
        } else {
            // Only an explicit click on a manually placed widget may start
            // AnyKeep.  The system-tray instance must stay passive while the
            // application is shutting down.
            if (!root.notesModel.inSystemTray && !root.notesModel.available) {
                root.notesModel.activate();
            }
            root.plasmoidItem.expanded = !root.plasmoidItem.expanded;
        }
    }

    Kirigami.Icon {
        anchors.fill: parent
        source: Plasmoid.icon
        isMask: true
        color: Kirigami.Theme.textColor
    }

    // KDE's Plasma applet runs outside AnyKeep's process; the status arrives
    // through the same D-Bus connectivity feed as other tray indicators.
    Rectangle {
        objectName: "plasmoidConnectivityBadge"
        visible: root.notesModel.available && root.notesModel.connectivityState !== 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.max(8, Math.min(parent.width, parent.height) * 0.34)
        height: width
        radius: width / 2
        border.width: 1
        border.color: Kirigami.Theme.backgroundColor
        color: root.notesModel.connectivityState === 3 ? "#2c995a"
             : root.notesModel.connectivityState === 2 ? "#b98a32"
             : root.notesModel.connectivityState === 4 ? "#c44a4a" : "#858b95"
    }
}
