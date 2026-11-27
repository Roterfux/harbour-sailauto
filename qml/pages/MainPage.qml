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
                visible: !controller.connected
                text: qsTr("Ton reparieren")
                onClicked: controller.repairAudio()
            }
            MenuItem {
                text: qsTr("Ziele")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("DestinationsPage.qml"))
            }
            MenuItem {
                text: qsTr("Apps fürs Auto")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("AppsPage.qml"))
            }
            MenuItem {
                text: qsTr("Protokoll")
                onClicked: pageStack.animatorPush(Qt.resolvedUrl("LogPage.qml"))
            }
            MenuItem {
                visible: controller.streaming
                text: qsTr("Tontest Ansage (3 Pieptöne)")
                onClicked: controller.soundTest("guidance")
            }
            MenuItem {
                visible: controller.streaming
                text: qsTr("Tontest Musik (60 s)")
                onClicked: controller.soundTest("media")
            }
            MenuItem {
                visible: controller.streaming
                text: qsTr("Tontest stoppen")
                onClicked: controller.stopSoundTest()
            }
            MenuItem {
                visible: controller.usbActive
                text: qsTr("USB neu verbinden")
                onClicked: controller.replugUsb()
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

            PageHeader {
                title: "SailAuto"
                description: qsTr("Version %1").arg(controller.version)
            }

            TextSwitch {
                text: qsTr("Auf Head Unit warten")
                description: controller.listening
                             ? qsTr("Port %1 ist offen").arg(controller.port)
                             : qsTr("Startet den Empfang für OpenAuto")
                checked: controller.listening
                automaticCheck: false
                onClicked: controller.listening ? controller.stopListening() : controller.startListening()
            }

            TextSwitch {
                text: qsTr("USB (Auto per Kabel)")
                description: qsTr("Schaltet USB in den Android-Accessory-Modus (wie ein Android-Telefon im Auto). "
                                  + "Dateiübertragung per USB ist solange aus.")
                checked: controller.usbActive
                automaticCheck: false
                onClicked: controller.usbActive ? controller.stopUsb() : controller.startUsb()
            }

            TextSwitch {
                text: qsTr("Autostart im Auto")
                description: qsTr("Startet SailAuto und den USB-Modus von selbst, wenn das Auto Android Auto "
                                  + "anfragt oder sich das Telefon per Bluetooth mit einem bekannten Auto verbindet. "
                                  + "Ist das Auto 60 s weg, wird USB wieder normal.")
                checked: controller.autostart
                onCheckedChanged: controller.autostart = checked
            }

            TextSwitch {
                text: qsTr("Benachrichtigungen im Auto")
                description: qsTr("Zeigt neue Benachrichtigungen als Banner im Auto. Der Text erscheint nur im Stand.")
                checked: controller.notifyToCar
                onCheckedChanged: controller.notifyToCar = checked
            }

            TextSwitch {
                text: qsTr("Wetter im Auto")
                description: qsTr("Oben im Auto-Bildschirm, antippen für die Vorhersage. Quelle: Open-Meteo; "
                                  + "dafür geht die ungefähre Position (auf ~10 km gerundet) ins Internet.")
                checked: controller.weatherEnabled
                onCheckedChanged: controller.weatherEnabled = checked
            }

            TextSwitch {
                text: qsTr("Telefon-Ton ins Auto")
                description: qsTr("Musik, Navigationsansagen usw. laufen über die Autolautsprecher, solange verbunden")
                checked: controller.soundToCar
                onCheckedChanged: controller.soundToCar = checked
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
                label: qsTr("Diagnose")
                value: controller.traceDir
                visible: controller.traceDir !== ""
            }
            DetailItem {
                label: qsTr("Zertifikat")
                value: controller.customCert ? qsTr("eigenes") : qsTr("Entwickler (selbstsigniert)")
            }

            SectionHeader {
                text: qsTr("Auto geparkt")
                visible: controller.parkedValid
            }
            DetailItem {
                visible: controller.parkedValid
                label: qsTr("Seit")
                value: controller.parkedTime
                       + (controller.parkedAccuracy >= 0 ? qsTr(" (±%1 m)").arg(Math.round(controller.parkedAccuracy)) : "")
            }
            Row {
                visible: controller.parkedValid
                x: Theme.horizontalPageMargin
                spacing: Theme.paddingMedium
                Button {
                    text: qsTr("Auf Karte zeigen")
                    onClicked: Qt.openUrlExternally("geo:" + controller.parkedLat.toFixed(6) + ","
                                                    + controller.parkedLon.toFixed(6))
                }
                Button {
                    text: qsTr("Kopieren")
                    onClicked: Clipboard.text = controller.parkedLat.toFixed(6) + ", " + controller.parkedLon.toFixed(6)
                }
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
