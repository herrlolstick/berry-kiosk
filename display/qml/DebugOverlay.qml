import QtQuick
import BerryKiosk

// Device, screen, render tier and frame rate, for setting up a kiosk (F1 / --debug)
Rectangle {
    id: overlay

    required property KioskController kiosk

    property int frames: 0
    property int fps: 0

    readonly property int fontSize: Math.max(12, Math.round((Window.window?.height ?? 720) / 60))

    width: info.implicitWidth + 2 * fontSize
    height: info.implicitHeight + 2 * fontSize
    radius: fontSize / 2
    color: "#b0000000"

    Text {
        id: info

        anchors.centerIn: parent
        color: "white"
        font.pixelSize: overlay.fontSize
        textFormat: Text.PlainText
        text: [
            "berry-kiosk " + Qt.application.version,
            "Gerät: " + overlay.kiosk.deviceModel,
            "Bildschirm: " + overlay.kiosk.screenSize.width + " × " + overlay.kiosk.screenSize.height,
            "Render-Stufe: " + overlay.kiosk.renderTier
                + " (Bilder bis " + overlay.kiosk.renderSize.width + " × " + overlay.kiosk.renderSize.height + ")",
            "Grund: " + overlay.kiosk.renderReason,
            "Max. Textur: " + (overlay.kiosk.maxTextureSize > 0 ? overlay.kiosk.maxTextureSize + " px" : "unbekannt"),
            "FPS: " + overlay.fps + " (nur während Animationen aussagekräftig)"
        ].join("\n")
    }

    // afterAnimating runs on the GUI thread once per frame, unlike frameSwapped
    Connections {
        target: overlay.Window.window
        enabled: overlay.visible

        function onAfterAnimating() {
            overlay.frames++
        }
    }

    Timer {
        interval: 1000
        repeat: true
        running: overlay.visible
        onTriggered: {
            overlay.fps = overlay.frames
            overlay.frames = 0
        }
    }
}
