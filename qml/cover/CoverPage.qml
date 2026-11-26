import QtQuick 2.2
import Sailfish.Silica 1.0

CoverBackground {
    Column {
        anchors.centerIn: parent
        width: parent.width - 2 * Theme.paddingLarge
        spacing: Theme.paddingMedium

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: "SailAuto"
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeLarge
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: controller.version
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: controller.status
            font.pixelSize: Theme.fontSizeSmall
        }
        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            visible: controller.streaming
            text: controller.fps.toFixed(0) + " fps"
            color: Theme.secondaryColor
            font.pixelSize: Theme.fontSizeExtraSmall
        }
    }

    CoverActionList {
        enabled: !controller.listening
        CoverAction {
            iconSource: "image://theme/icon-cover-play"
            onTriggered: controller.startListening()
        }
    }
    CoverActionList {
        enabled: controller.listening
        CoverAction {
            iconSource: "image://theme/icon-cover-cancel"
            onTriggered: controller.stopListening()
        }
    }
}
