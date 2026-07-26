pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    signal backRequested()
    signal draftOpened()

    property double discardRequestId: 0
    property string pendingDiscardId: ""

    function requestDiscard(draftId, title) {
        pendingDiscardId = draftId
        discardRequestId = mobileApp.dialogs.confirm(
                    qsTr("Discard draft"),
                    qsTr("Discard the draft “%1”? This cannot be undone.").arg(title),
                    qsTr("Discard"), qsTr("Cancel"), true)
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent

            ToolButton {
                text: qsTr("‹")
                font.pixelSize: 27
                Accessible.name: qsTr("Back")
                onClicked: root.backRequested()
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Drafts")
                font.pixelSize: 20
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
            }

            Item { Layout.preferredWidth: 40 }
        }
    }

    ListView {
        id: draftsList
        anchors.fill: parent
        anchors.margins: 8
        clip: true
        spacing: 4
        model: mobileApp.recoverableDrafts

        delegate: ItemDelegate {
            id: delegate

            required property var modelData

            width: draftsList.width
            implicitHeight: contents.implicitHeight + 20
            rightPadding: discardButton.width + 16

            contentItem: ColumnLayout {
                id: contents
                spacing: 3

                Label {
                    Layout.fillWidth: true
                    text: delegate.modelData.title
                    font.bold: true
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: delegate.modelData.preview
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    opacity: 0.78
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2").arg(delegate.modelData.storageName).arg(delegate.modelData.updated)
                    elide: Text.ElideRight
                    opacity: 0.62
                    font.pixelSize: 12
                }

                Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: delegate.modelData.lastError
                    color: palette.brightText
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.pixelSize: 12
                }
            }

            ToolButton {
                id: discardButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                display: AbstractButton.IconOnly
                contentItem: Image {
                    source: "image://qtnoteicons/user-trash-full-symbolic/user-trash-full-symbolic.svg/auto"
                    sourceSize.width: 20
                    sourceSize.height: 20
                    fillMode: Image.PreserveAspectFit
                }
                Accessible.name: qsTr("Discard draft")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: root.requestDiscard(delegate.modelData.draftId, delegate.modelData.title)
            }

            onClicked: {
                if (mobileApp.openDraft(delegate.modelData.draftId))
                    root.draftOpened()
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: draftsList.count === 0
        text: qsTr("No recoverable drafts")
        opacity: 0.7
    }

    Connections {
        target: mobileApp.dialogs
        function onCompleted(requestId, accepted) {
            if (requestId !== root.discardRequestId)
                return
            root.discardRequestId = 0
            if (accepted)
                mobileApp.discardDraft(root.pendingDiscardId)
            root.pendingDiscardId = ""
        }
    }
}
