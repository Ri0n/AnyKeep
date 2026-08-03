/*
    SPDX-License-Identifier: GPL-3.0-only
*/

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import org.kde.kirigami as Kirigami
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import QtQuick.Layouts
import plasma.applet.com.github.ri0n.anykeep 1.0 as AnyKeep

PlasmoidItem {
    id: root

    switchWidth: Kirigami.Units.gridUnit * 16
    switchHeight: Kirigami.Units.gridUnit * 12

    Plasmoid.icon: "anykeep-symbolic"
    Plasmoid.status: {
        if (!Plasmoid.inSystemTray) {
            return PlasmaCore.Types.ActiveStatus;
        }

        return notesModel.available
            ? PlasmaCore.Types.ActiveStatus
            : PlasmaCore.Types.HiddenStatus;
    }

    toolTipMainText: qsTr("Notes")
    toolTipSubText: notesModel.available
        ? notesModel.count === 1
            ? qsTr("1 recent note")
            : notesModel.hasMore
                ? qsTr("%1+ recent notes").arg(notesModel.count)
                : qsTr("%1 recent notes").arg(notesModel.count)
        : qsTr("AnyKeep is not running")

    onExpandedChanged: {
        if (root.expanded) {
            notesModel.refresh();
        }
    }

    AnyKeep.NotesModel {
        id: notesModel
        inSystemTray: Plasmoid.inSystemTray
    }

    compactRepresentation: CompactRepresentation {
        plasmoidItem: root
        notesModel: notesModel
    }

    fullRepresentation: FullRepresentation {
        id: fullRepresentation

        plasmoidItem: root
        notesModel: notesModel
        inSystemTray: Plasmoid.inSystemTray
        newNoteAction: newNoteAction
        noteManagerAction: noteManagerAction
        configureAction: configureAction

        Layout.minimumWidth: Kirigami.Units.gridUnit * 16
        Layout.preferredWidth: Kirigami.Units.gridUnit * 22

        Layout.minimumHeight: Kirigami.Units.gridUnit * 16
        Layout.preferredHeight: Kirigami.Units.gridUnit * 22
    }

    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            id: newNoteAction
            text: qsTr("New Note")
            icon.name: "list-add-symbolic"
            priority: PlasmaCore.Action.HighPriority
            onTriggered: {
                notesModel.createNote(root.Window.window);
                root.expanded = false;
            }
        },
        PlasmaCore.Action {
            id: configureAction
            text: qsTr("Configure AnyKeep...")
            icon.name: "configure"
            onTriggered: notesModel.showOptions(root.Window.window)
        },
        PlasmaCore.Action {
            id: noteManagerAction
            text: qsTr("Note Manager")
            icon.name: "view-list-details"
            onTriggered: notesModel.showNoteManager(root.Window.window)
        },
        PlasmaCore.Action {
            text: qsTr("About AnyKeep")
            icon.name: "help-about"
            onTriggered: notesModel.showAbout(root.Window.window)
        },
        PlasmaCore.Action {
            text: qsTr("Close AnyKeep")
            icon.name: "window-close"
            visible: notesModel.available
            onTriggered: notesModel.quit()
        }
    ]
}
