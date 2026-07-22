pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: window

    width: 420
    height: 760
    visible: true
    title: qsTr("QtNote")

    StackView {
        id: navigation
        anchors.fill: parent
        initialItem: RootPage {
            onOpenPluginSettings: (pluginId, pluginName) => navigation.push(pluginSettings, {
                "pluginId": pluginId,
                "pluginName": pluginName
            })
            onOpenStorageSettings: (storageId, storageName) => navigation.push(storageSettings, {
                "storageId": storageId,
                "storageName": storageName
            })
        }
    }

    DialogHost {
        dialogService: mobileApp.dialogs
    }

    Connections {
        target: mobileApp
        function onCurrentNoteEditorChanged() {
            if (mobileApp.currentNoteEditor && navigation.depth === 1)
                navigation.push(noteEditor, { "editor": mobileApp.currentNoteEditor })
        }
        function onOperationFailed(message) {
            mobileApp.dialogs.inform(qsTr("QtNote"), message)
        }
        function onOperationCompleted(message) {
            mobileApp.dialogs.inform(qsTr("Done"), message)
        }
    }

    Connections {
        target: Qt.application
        function onStateChanged() {
            if (Qt.application.state === Qt.ApplicationActive) {
                mobileApp.processPendingLaunchIntent()
                return
            }
            if (!mobileApp.currentNoteEditor)
                return
            const page = navigation.currentItem
            if (page && typeof page.checkpointEditor === "function")
                page.checkpointEditor()
            else
                mobileApp.saveCurrentNote()
        }
    }

    Component.onCompleted: {
        mobileApp.processPendingLaunchIntent()
        if (mobileApp.currentNoteEditor && navigation.depth === 1)
            navigation.push(noteEditor, { "editor": mobileApp.currentNoteEditor })
    }

    onClosing: function(close) {
        if (navigation.depth <= 1)
            return
        close.accepted = false
        const page = navigation.currentItem
        if (page && typeof page.closeEditor === "function")
            page.closeEditor()
        else
            navigation.pop()
    }

    Component {
        id: noteEditor
        NoteEditorPage {
            onBackRequested: navigation.pop()
        }
    }

    Component {
        id: pluginSettings
        PluginSettingsPage {
            onBackRequested: navigation.pop()
        }
    }

    Component {
        id: storageSettings
        StorageSettingsPage {
            onBackRequested: navigation.pop()
        }
    }
}
