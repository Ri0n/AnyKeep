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
}
