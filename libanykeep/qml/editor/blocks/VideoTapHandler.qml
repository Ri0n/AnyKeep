import QtQuick

TapHandler {
    id: handler
    acceptedButtons: Qt.LeftButton
    signal clicked()
    signal doubleClicked()

    // Give mouse double clicks precedence over play/pause on Qt 6.4 too.
    // Touch taps can play or pause immediately.
    onTapped: function(point, button) {
        if (button !== Qt.LeftButton) {
            clicked()
        } else if (tapCount === 2 && singleClick.running) {
            singleClick.stop()
            doubleClicked()
        } else {
            if (singleClick.running)
                clicked()
            singleClick.restart()
        }
    }

    onEnabledChanged: if (!enabled) singleClick.stop()

    property Timer singleClick: Timer {
        interval: Qt.styleHints.mouseDoubleClickInterval
        onTriggered: handler.clicked()
    }
}
