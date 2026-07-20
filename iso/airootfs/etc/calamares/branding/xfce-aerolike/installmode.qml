import io.calamares.core 1.0
import io.calamares.ui 1.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    width: parent.width
    height: parent.height

    ColumnLayout {
        anchors.margins: 20
        anchors.fill: parent
        spacing: 12

        Label {
            text: "<b>Select install mode</b>"
            font.pixelSize: 14
        }

        Label {
            text: "Choose how the system image is sourced for installation:"
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        RadioButton {
            id: offlineBtn
            text: "Offline - use image bundled in the ISO (no network required)"
            checked: true
            onCheckedChanged: {
                if (checked) Global.insert("installMode", "offline")
            }
        }

        RadioButton {
            id: onlineBtn
            text: "Online - pull the latest image from the registry"
            onCheckedChanged: {
                if (checked) Global.insert("installMode", "online")
            }
        }

        ButtonGroup {
            buttons: [offlineBtn, onlineBtn]
        }

        Item {
            Layout.fillHeight: true
        }
    }

    Component.onCompleted: {
        Global.insert("installMode", "offline")
    }
}
