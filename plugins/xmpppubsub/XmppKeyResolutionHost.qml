pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property Item hostItem
    required property var controller
    property bool standalone: false
    property int pendingRemoveRow: -1
    property var pendingRemoveDeviceId: 0
    property string pendingRemoveLabel: ""

    parent: hostItem
    anchors.fill: parent
    z: 100000

    function pageTitle() {
        switch (controller.currentPage) {
        case 0:
            return controller.localKeyMissing ? qsTr("Set up encrypted XMPP notes") : qsTr("AnyKeep found incompatible storage keys");
        case 1:
            return qsTr("Verify your AnyKeep devices");
        case 2:
            return qsTr("Choose the key to keep");
        case 3:
            return root.controller.freshStart ? qsTr("Review new storage") : qsTr("Review synchronization");
        case 4:
            return qsTr("Synchronization result");
        default:
            return "";
        }
    }

    function pageSubtitle() {
        switch (controller.currentPage) {
        case 0:
            return controller.localKeyMissing ? qsTr("Choose whether to use existing encrypted notes or start with a new key.") : qsTr("Notes or another AnyKeep device use a different encryption key.");
        case 1:
            return qsTr("Select devices you recognize. Their fingerprints are used only to establish encrypted OMEMO sessions.");
        case 2:
            return qsTr("AnyKeep grouped existing notes and online devices by their storage-key fingerprint.");
        case 3:
            return controller.freshStart ? qsTr("AnyKeep will start with a new empty storage without changing the old encrypted notes.") : qsTr("AnyKeep will verify accessible notes and re-encrypt only records using another key.");
        case 4:
            return "";
        default:
            return "";
        }
    }

    Dialog {
        id: dialog

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.max(280, Math.min(760, root.width - 16))
        height: Math.max(300, Math.min(720, root.height - 16))
        modal: true
        closePolicy: Popup.NoAutoClose
        title: root.controller.localKeyMissing ? qsTr("Set up XMPP notes") : qsTr("Sync XMPP notes")

        contentItem: Item {
            id: dialogContentHost
        }

        onClosed: {
            if (!root.controller.completed)
                root.controller.cancel();
        }
    }

    Pane {
        id: standaloneSurface

        anchors.fill: parent
        visible: root.standalone
        padding: 16

        contentItem: Item {
            id: standaloneContentHost
        }
    }

    ColumnLayout {
        id: surface

        parent: root.standalone ? standaloneContentHost : dialogContentHost
        anchors.fill: parent
        spacing: 10

        ScrollView {
            id: recoveryScroll
            objectName: "xmppRecoveryScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            Layout.preferredHeight: 0
            contentWidth: availableWidth
            contentHeight: body.height
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Column {
                id: body
                width: recoveryScroll.availableWidth
                spacing: 10

                ColumnLayout {
                    id: heading
                    width: body.width
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            text: root.pageTitle()
                            font.pixelSize: 18
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            text: qsTr("%1 / %2").arg(root.controller.currentPage + 1).arg(root.controller.pageCount)
                            color: palette.placeholderText
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.pageSubtitle()
                        wrapMode: Text.WordWrap
                        color: palette.placeholderText
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: palette.mid
                        opacity: 0.35
                    }
                }

                StackLayout {
                    id: pages

                    width: body.width
                    height: Math.max(180, currentIndex === 1 ? devicePage.implicitHeight : currentIndex === 2 ? keysPage.implicitHeight : 0, recoveryScroll.availableHeight - heading.implicitHeight - body.spacing - (syncProgress.visible ? syncProgress.implicitHeight + body.spacing : 0))
                    clip: true
                    currentIndex: root.controller.currentPage

                    ScrollView {
                        id: introductionScroll

                        clip: true

                        ScrollBar.horizontal: ScrollBar {
                            policy: ScrollBar.AlwaysOff
                        }

                        ColumnLayout {
                            width: introductionScroll.availableWidth
                            spacing: 14

                            Label {
                                Layout.fillWidth: true
                                text: root.controller.localKeyMissing ? qsTr("Is this your first AnyKeep device? Start with a new key. If you already used AnyKeep with this XMPP account, you can recover your existing notes instead.") : qsTr("This recovery flow locates other AnyKeep devices, collects their keys, and safely moves accessible notes to one key you choose.")
                                wrapMode: Text.WordWrap
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                visible: root.controller.localKeyMissing

                                Button {
                                    objectName: "xmppRecoverExistingNotesButton"
                                    Layout.fillWidth: true
                                    text: qsTr("Use existing notes")
                                    highlighted: true
                                    enabled: root.controller.canGoNext
                                    onClicked: root.controller.next()
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Find a key from another device where AnyKeep is already set up.")
                                    wrapMode: Text.WordWrap
                                    color: palette.placeholderText
                                }

                                Button {
                                    objectName: "xmppStartNewStorageButton"
                                    Layout.fillWidth: true
                                    text: qsTr("Start with a new key")
                                    enabled: root.controller.canStartFresh
                                    onClicked: startFreshDialog.open()
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Use a new empty storage on this device. Existing encrypted notes, if any, cannot be read without their original key.")
                                    wrapMode: Text.WordWrap
                                    color: palette.placeholderText
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: text.length > 0
                                    text: root.controller.keyStatus
                                    wrapMode: Text.WordWrap
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("No existing notes are deleted or overwritten while you choose. You can cancel and return to setup later.")
                                wrapMode: Text.WordWrap
                                color: palette.placeholderText
                            }
                        }
                    }

                    ColumnLayout {
                        id: devicePage
                        spacing: 8

                        ListView {
                            id: devicesView

                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 180
                            clip: true
                            spacing: 4
                            model: root.controller.devicesModel
                            boundsBehavior: Flickable.StopAtBounds
                            reuseItems: true

                            ScrollBar.vertical: ScrollBar {
                                id: devicesScrollBar
                                policy: ScrollBar.AsNeeded
                            }

                            delegate: Frame {
                                id: deviceDelegate

                                required property int index
                                required property string label
                                required property string fingerprint
                                required property string trustText
                                required property bool selected
                                required property bool selectable
                                required property bool trusted
                                required property var deviceId

                                width: Math.max(0, ListView.view.width - devicesScrollBar.width - 6)
                                padding: 6

                                contentItem: RowLayout {
                                    id: deviceRow
                                    spacing: 8

                                    CheckBox {
                                        Layout.alignment: Qt.AlignTop
                                        checked: deviceDelegate.selected
                                        enabled: deviceDelegate.selectable && !root.controller.busy
                                        onClicked: root.controller.setDeviceSelected(deviceDelegate.index, checked)
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1

                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("%1 — OMEMO ID %2").arg(deviceDelegate.label).arg(deviceDelegate.deviceId)
                                            font.bold: true
                                            elide: Text.ElideRight
                                            maximumLineCount: 1
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: deviceDelegate.fingerprint
                                            wrapMode: Text.WordWrap
                                            color: palette.placeholderText
                                            font.family: "monospace"
                                            font.pixelSize: 11
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: deviceDelegate.trustText
                                            color: deviceDelegate.trusted ? palette.text : palette.placeholderText
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                            maximumLineCount: 1
                                        }
                                    }

                                    ToolButton {
                                        Layout.alignment: Qt.AlignTop
                                        text: qsTr("Remove")
                                        enabled: !root.controller.busy
                                        onClicked: {
                                            root.pendingRemoveRow = deviceDelegate.index;
                                            root.pendingRemoveDeviceId = deviceDelegate.deviceId;
                                            root.pendingRemoveLabel = deviceDelegate.label;
                                            removeDeviceDialog.open();
                                        }
                                    }
                                }
                            }

                            Label {
                                anchors.centerIn: parent
                                visible: devicesView.count === 0
                                width: Math.min(parent.width - 24, 420)
                                horizontalAlignment: Text.AlignHCenter
                                text: qsTr("No previously published OMEMO device identities found. This list is separate from online XMPP resources; bring an existing AnyKeep device online.")
                                wrapMode: Text.WordWrap
                                color: palette.placeholderText
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.controller.deviceStatus
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.controller.presenceStatus
                            visible: text.length > 0
                            wrapMode: Text.WordWrap
                            color: palette.placeholderText
                        }

                        Repeater {
                            model: root.controller.onlineResources

                            delegate: Label {
                                required property string modelData
                                Layout.fillWidth: true
                                text: root.controller.refusedResources.indexOf(modelData) >= 0
                                      ? qsTr("%1 — explicitly declined trust").arg(modelData)
                                      : qsTr("%1 — online").arg(modelData)
                                wrapMode: Text.WordWrap
                                color: palette.placeholderText
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: root.controller.waitingForDevices
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                font.bold: true
                                text: qsTr("Waiting for another AnyKeep device")
                                wrapMode: Text.WordWrap
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Open AnyKeep on a device that already has your notes. When a new XMPP resource appears online, AnyKeep will request the key automatically. A resource that stays online will not receive repeated requests. You can also leave setup without changing the key.")
                                wrapMode: Text.WordWrap
                                color: palette.placeholderText
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Button {
                                    objectName: "xmppRecoveryCheckNowButton"
                                    Layout.fillWidth: true
                                    text: qsTr("Check again")
                                    enabled: !root.controller.busy
                                    onClicked: root.controller.checkAgain()
                                }

                                Button {
                                    objectName: "xmppRecoveryNewKeyWhileWaitingButton"
                                    Layout.fillWidth: true
                                    visible: root.controller.canStartFresh
                                    text: qsTr("Start with a new key")
                                    enabled: !root.controller.busy
                                    onClicked: startFreshDialog.open()
                                }
                            }

                            Button {
                                objectName: "xmppRecoveryContinueWithoutXmppButton"
                                Layout.fillWidth: true
                                text: root.hostItem && root.hostItem.window
                                      && typeof root.hostItem.window.disableXmppStorageAfterRecovery === "function"
                                      ? qsTr("Disable XMPP storage and continue")
                                      : qsTr("Close setup without changing XMPP")
                                enabled: !root.controller.busy
                                onClicked: {
                                    const hostWindow = root.hostItem ? root.hostItem.window : null;
                                    const disable = hostWindow && typeof hostWindow.disableXmppStorageAfterRecovery === "function";
                                    if (disable)
                                        hostWindow.disableXmppStorageAfterRecovery();
                                    root.controller.cancel();
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("When you request the storage key, check another AnyKeep device logged into the same XMPP account. Approve the device-trust prompt there to allow key synchronization.")
                            wrapMode: Text.WordWrap
                            color: palette.placeholderText
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("OMEMO identities may be offline. Online status is shown only for XMPP resources advertising AnyKeep key synchronization; XMPP does not reliably map a resource to an OMEMO device ID.")
                            wrapMode: Text.WordWrap
                            color: palette.placeholderText
                            font.pixelSize: 12
                        }
                    }

                    ColumnLayout {
                        id: keysPage
                        spacing: 8

                        ListView {
                            id: keysView

                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 180
                            visible: count > 0
                            clip: true
                            spacing: 4
                            model: root.controller.keysModel
                            boundsBehavior: Flickable.StopAtBounds
                            reuseItems: true

                            ScrollBar.vertical: ScrollBar {
                                id: keysScrollBar
                                policy: ScrollBar.AsNeeded
                            }

                            delegate: Frame {
                                id: keyDelegate

                                required property int index
                                required property string fingerprint
                                required property string source
                                required property int noteCount
                                required property string status
                                required property bool available

                                width: Math.max(0, ListView.view.width - keysScrollBar.width - 6)
                                // Keep descriptions legible for unavailable keys. The
                                // disabled radio control already conveys that a key
                                // cannot be selected; dimming the whole Frame also
                                // fades its explanatory text below readable contrast.
                                padding: 6

                                contentItem: RowLayout {
                                    spacing: 8

                                    RadioButton {
                                        checked: root.controller.selectedKeyIndex === keyDelegate.index
                                        enabled: keyDelegate.available && !root.controller.busy
                                        onClicked: root.controller.selectKey(keyDelegate.index)
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Label {
                                            Layout.fillWidth: true
                                            text: keyDelegate.fingerprint
                                            color: keyDelegate.available ? palette.text : palette.placeholderText
                                            font.bold: true
                                            font.family: "monospace"
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                            maximumLineCount: 1
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Available from: %1").arg(keyDelegate.source)
                                            color: keyDelegate.available ? palette.text : palette.placeholderText
                                            wrapMode: Text.WordWrap
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Notes: %1 — %2").arg(keyDelegate.noteCount).arg(keyDelegate.status)
                                            color: palette.placeholderText
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 180
                            visible: keysView.count === 0
                            spacing: 10

                            Item {
                                Layout.fillHeight: true
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("No storage key was found")
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: true
                                font.pixelSize: 16
                            }

                            Label {
                                Layout.fillWidth: true
                                text: root.controller.canCreateNewKey ? qsTr("No notes are published in XMPP yet, so it is safe to create a new key for this account.") : qsTr("Start AnyKeep on a device that still has access to your notes, then try again. If you saved a recovery key, return to XMPP settings and import it on this device. If the old key is permanently lost, you can start with an empty storage instead.")
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                color: palette.placeholderText
                            }

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 8

                                Button {
                                    text: qsTr("Try again")
                                    enabled: !root.controller.busy
                                    onClicked: root.controller.retryKeySearch()
                                }

                                Button {
                                    visible: root.controller.canCreateNewKey
                                    text: qsTr("Create a new storage key")
                                    highlighted: true
                                    enabled: !root.controller.busy
                                    onClicked: root.controller.createNewKey()
                                }
                            }

                            Button {
                                Layout.alignment: Qt.AlignHCenter
                                visible: root.controller.canStartFresh && !root.controller.canCreateNewKey
                                text: qsTr("I no longer have the old key")
                                enabled: !root.controller.busy
                                onClicked: startFreshDialog.open()
                            }

                            Item {
                                Layout.fillHeight: true
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.controller.keyStatus
                            wrapMode: Text.WordWrap
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: keysView.count > 0
                            text: qsTr("Usually you should keep the key that owns the most notes. A key marked unavailable cannot be selected; bring one of its devices online or import its recovery key first.")
                            wrapMode: Text.WordWrap
                            color: palette.placeholderText
                        }
                    }

                    ScrollView {
                        id: summaryScroll

                        clip: true

                        ScrollBar.horizontal: ScrollBar {
                            policy: ScrollBar.AlwaysOff
                        }

                        Label {
                            width: summaryScroll.availableWidth
                            text: root.controller.summary
                            wrapMode: Text.WordWrap
                        }
                    }

                    ScrollView {
                        id: resultScroll

                        clip: true

                        ScrollBar.horizontal: ScrollBar {
                            policy: ScrollBar.AlwaysOff
                        }

                        Label {
                            width: resultScroll.availableWidth
                            text: root.controller.resultText
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                XmppSyncProgress {
                    id: syncProgress
                    objectName: "xmppRecoveryProgress"
                    width: body.width
                    summary: root.controller.syncSummary
                    events: root.controller.syncEvents
                    busy: root.controller.busy
                    completed: root.controller.syncCompleted
                    total: root.controller.syncTotal
                    logHeight: Math.max(64, Math.min(150, surface.height * 0.22))
                }
            }
        }

        GridLayout {
            objectName: "xmppRecoveryControls"
            Layout.fillWidth: true
            Layout.minimumHeight: implicitHeight
            columns: surface.width < 420 ? 2 : 3
            columnSpacing: 8
            rowSpacing: 8

            Button {
                objectName: "xmppRecoveryCancelButton"
                Layout.fillWidth: true
                text: qsTr("Cancel")
                enabled: root.controller.canCancel
                onClicked: root.controller.cancel()
            }

            Button {
                objectName: "xmppRecoveryBackButton"
                Layout.fillWidth: true
                text: qsTr("Back")
                enabled: root.controller.canGoBack
                onClicked: root.controller.back()
            }

            Button {
                objectName: "xmppRecoveryNextButton"
                Layout.fillWidth: true
                Layout.columnSpan: surface.width < 420 ? 2 : 1
                text: root.controller.nextText
                highlighted: true
                visible: !root.controller.localKeyMissing || root.controller.currentPage !== 0
                enabled: root.controller.canGoNext
                onClicked: root.controller.next()
            }
        }
    }

    Connections {
        target: root.controller

        function onFinished(accepted) {
            if (!root.standalone)
                dialog.close();

            // The mobile shell owns its navigation stack. After successful
            // XMPP setup, leave Settings and display the notes. Desktop and
            // standalone hosts have no such action and are left unchanged.
            // Cancelled/incomplete recovery must preserve the current page.
            if (accepted && !root.standalone && root.hostItem && root.hostItem.window) {
                const window = root.hostItem.window;
                if (typeof window.showNotesAfterXmppKeyResolution === "function")
                    window.showNotesAfterXmppKeyResolution();
            }
        }
    }

    Dialog {
        id: removeDeviceDialog

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Remove OMEMO device")
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: Label {
            width: Math.min(460, root.width - 40)
            text: qsTr("Remove %1 (OMEMO ID %2) from this XMPP account and delete its published bundle? Do this only for an old or lost client. Notes and the storage key are not deleted.").arg(root.pendingRemoveLabel).arg(root.pendingRemoveDeviceId)
            wrapMode: Text.WordWrap
        }

        onAccepted: {
            root.controller.removeDevice(root.pendingRemoveRow);
            root.pendingRemoveRow = -1;
        }
        onRejected: root.pendingRemoveRow = -1
    }

    Dialog {
        id: startFreshDialog

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Create a new XMPP storage key?")
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: Label {
            width: Math.min(460, root.width - 40)
            text: qsTr("A new key will be created for this device. Any existing encrypted XMPP notes will remain untouched but cannot be read with the new key. If you already have AnyKeep notes on another device, choose 'Use existing notes' instead. You can recover the old notes later if you still have their original key. Save your new recovery key after setup. Continue?")
            wrapMode: Text.WordWrap
        }

        onAccepted: root.controller.startFresh()
    }

    Component.onCompleted: {
        if (!root.standalone)
            dialog.open();
    }
}
