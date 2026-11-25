import QtQuick 2.2
import Sailfish.Silica 1.0
import Nemo.KeepAlive 1.2

Page {
    id: page
    allowedOrientations: Orientation.All

    // Keep CPU and network alive while a head unit is connected,
    // also with the phone display off.
    KeepAlive { enabled: controller.connected }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        PullDownMenu {
            MenuItem {
                text: qsTr("Protokoll")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("LogPage.qml"))
            }
            MenuItem {
                visible: controller.connected
                text: qsTr("Trennen")
                onClicked: controller.disconnectHeadUnit()
            }
        }

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: "SailAuto" }

            TextSwitch {
                text: qsTr("Auf Head Unit warten")
                description: controller.listening
                             ? qsTr("Port %1 ist offen").arg(controller.port)
                             : qsTr("Startet den Empfang für OpenAuto")
                checked: controller.listening
                automaticCheck: false
                onClicked: controller.listening ? controller.stopListening() : controller.startListening()
            }

            DetailItem { label: qsTr("Status"); value: controller.status }
            DetailItem {
                label: qsTr("Head Unit")
                value: controller.headUnit
                visible: controller.headUnit !== ""
            }
            DetailItem {
                label: qsTr("Video")
                value: controller.fps.toFixed(1) + " fps · " + controller.kbps.toFixed(0) + " kbit/s"
                visible: controller.streaming
            }
            DetailItem {
                label: qsTr("Zertifikat")
                value: controller.customCert ? qsTr("eigenes") : qsTr("Entwickler (selbstsigniert)")
            }

            SectionHeader { text: qsTr("So verbindest du OpenAuto") }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Telefon und OpenAuto ins selbe WLAN (oder Hotspot des Telefons). "
                           + "In OpenAuto auf „Wireless“ / „Connect to device“ tippen und diese Adresse eingeben:")
            }
            Repeater {
                model: controller.addresses
                Label {
                    x: Theme.horizontalPageMargin
                    text: modelData
                    color: Theme.highlightColor
                    font.pixelSize: Theme.fontSizeLarge
                }
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Eigenes Zertifikat: cert.pem und key.pem nach %1 kopieren und neu verbinden.")
                      .arg(controller.certDir)
            }
        }
        VerticalScrollDecorator {}
    }
}
