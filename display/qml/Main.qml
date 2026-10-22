import QtQuick
import BerryKiosk

Window {
    id: window

    required property KioskController kiosk
    required property bool windowed

    width: 1280
    height: 720
    visibility: windowed ? Window.Windowed : Window.FullScreen
    visible: true
    color: "black"
    title: qsTr("berry-kiosk")

    // The layout is drawn on this canvas; rotated for screens mounted in portrait
    Item {
        id: canvas

        readonly property bool portrait: window.kiosk.rotation === 90 || window.kiosk.rotation === 270

        anchors.centerIn: parent
        width: portrait ? parent.height : parent.width
        height: portrait ? parent.width : parent.height
        rotation: window.kiosk.rotation

        LayoutView {
            anchors.fill: parent
            kiosk: window.kiosk
        }

        DebugOverlay {
            id: debugOverlay

            anchors {
                left: parent.left
                top: parent.top
                margins: Math.round(canvas.height / 60)
            }
            kiosk: window.kiosk
            visible: window.kiosk.debug
        }
    }

    // Keys only work while developing; a kiosk must not be closable from a keyboard
    Shortcut {
        sequence: "F1"
        enabled: window.windowed || window.kiosk.debug
        onActivated: debugOverlay.visible = !debugOverlay.visible
    }

    Shortcut {
        sequences: [StandardKey.Quit, "Esc"]
        enabled: window.windowed || window.kiosk.debug
        onActivated: Qt.quit()
    }
}
