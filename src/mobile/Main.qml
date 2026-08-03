pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: window

    width: 420
    height: 760
    visible: true
    title: qsTr("AnyKeep")

    StackView {
        id: navigation
        anchors.fill: parent
        initialItem: RootPage {
            onOpenSettings: navigation.push(settingsHub)
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
            mobileApp.dialogs.inform(qsTr("AnyKeep"), message)
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
        id: settingsHub
        SettingsHubPage {
            onBackRequested: navigation.pop()
            onOpenGeneral: navigation.push(appSettings)
            onOpenDrafts: navigation.push(draftsPage)
            onOpenStorages: navigation.push(storagesSettings)
            onOpenPlugins: navigation.push(pluginsSettings)
        }
    }

    Component {
        id: draftsPage
        DraftsPage {
            onBackRequested: navigation.pop()
            onDraftOpened: {
                navigation.pop(null, StackView.Immediate)
                navigation.push(noteEditor, { "editor": mobileApp.currentNoteEditor })
            }
        }
    }

    Component {
        id: appSettings
        AppSettingsPage { onBackRequested: navigation.pop() }
    }

    Component {
        id: storagesSettings
        StoragesPage {
            onBackRequested: navigation.pop()
            onOpenSettings: (storageId, storageName) => navigation.push(storageSettings, {
                "storageId": storageId,
                "storageName": storageName
            })
        }
    }

    Component {
        id: pluginsSettings
        PluginsPage {
            onBackRequested: navigation.pop()
            onOpenSettings: (pluginId, pluginName) => navigation.push(pluginSettings, {
                "pluginId": pluginId,
                "pluginName": pluginName
            })
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
