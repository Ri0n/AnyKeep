pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: page

    signal openSettings(string storageId, string storageName)

    ListView {
        id: storagesView
        anchors.fill: parent
        anchors.margins: 12
        clip: true
        model: mobileApp.storagesModel
        spacing: 2

        move: Transition {
            NumberAnimation { properties: "y"; duration: 140; easing.type: Easing.OutCubic }
        }

        delegate: ItemDelegate {
            id: storageDelegate

            required property int index
            required property string storageId
            required property string name
            required property bool accessible
            required property bool configurable
            required property string tooltip
            required property string iconSource

            width: storagesView.width
            implicitHeight: Math.max(64, contentItem.implicitHeight + topPadding + bottomPadding)
            // Accessibility describes whether notes can currently be read or
            // written. It must not disable priority controls or settings for an
            // unconfigured/offline storage.
            enabled: true
            rightPadding: actionRow.width + 12
            onClicked: {
                if (configurable)
                    page.openSettings(storageId, name)
            }

            contentItem: RowLayout {
                spacing: 10

                Item {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.alignment: Qt.AlignVCenter

                    Image {
                        id: storageIcon
                        anchors.fill: parent
                        source: storageDelegate.iconSource
                        sourceSize.width: 28
                        sourceSize.height: 28
                        fillMode: Image.PreserveAspectFit
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: storageIcon.status !== Image.Ready
                        text: "▣"
                        font.pixelSize: 20
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: storageDelegate.name
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        text: storageDelegate.tooltip.length > 0
                              ? storageDelegate.tooltip
                              : (storageDelegate.accessible ? "" : qsTr("Not accessible"))
                        visible: text.length > 0
                        elide: Text.ElideRight
                        color: palette.mid
                        font.pixelSize: 13
                    }
                }
            }

            Row {
                id: actionRow
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Column {
                    visible: storagesView.count > 1
                    spacing: 0

                    ToolButton {
                        width: 34
                        height: 28
                        padding: 0
                        text: "▲"
                        font.pixelSize: 13
                        enabled: storageDelegate.index > 0
                        Accessible.name: qsTr("Move %1 up").arg(storageDelegate.name)
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: mobileApp.moveStorage(storageDelegate.index,
                                                          storageDelegate.index - 1)
                    }

                    ToolButton {
                        width: 34
                        height: 28
                        padding: 0
                        text: "▼"
                        font.pixelSize: 13
                        enabled: storageDelegate.index + 1 < storagesView.count
                        Accessible.name: qsTr("Move %1 down").arg(storageDelegate.name)
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: mobileApp.moveStorage(storageDelegate.index,
                                                          storageDelegate.index + 1)
                    }
                }

                ToolButton {
                    id: settingsButton
                    width: 40
                    height: 40
                    anchors.verticalCenter: parent.verticalCenter
                    visible: storageDelegate.configurable
                    enabled: storageDelegate.configurable
                    padding: 0
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Settings")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name

                    // image:// providers are loaded reliably by Image. Passing the
                    // same URL through AbstractButton.icon.source is treated as a
                    // local file by some Qt Quick Controls builds.
                    contentItem: Image {
                        width: 22
                        height: 22
                        anchors.centerIn: parent
                        source: "image://qtnoteicons/configure/preferences-system-symbolic.svg"
                        sourceSize.width: 22
                        sourceSize.height: 22
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        opacity: settingsButton.enabled ? 1.0 : 0.38
                    }

                    onClicked: page.openSettings(storageDelegate.storageId, storageDelegate.name)
                }
            }
        }

        Label {
            anchors.centerIn: parent
            width: Math.min(parent.width - 32, 340)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: storagesView.count === 0
            text: qsTr("No note storages are available.")
            color: palette.mid
        }
    }
}
