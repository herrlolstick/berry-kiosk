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

        // Layout on screen; switches go through a short fade to black. With the
        // screen off nothing is loaded at all, so nothing renders.
        property var shownLayout: window.kiosk.screenOff ? ({}) : window.kiosk.layout

        LayoutView {
            anchors.fill: parent
            kiosk: window.kiosk
            layout: canvas.shownLayout
        }

        Rectangle {
            id: curtain

            anchors.fill: parent
            color: "black"
            opacity: 0
            z: 10
        }

        SequentialAnimation {
            id: layoutSwitch

            NumberAnimation { target: curtain; property: "opacity"; to: 1; duration: 400; easing.type: Easing.InQuad }
            ScriptAction { script: canvas.shownLayout = window.kiosk.screenOff ? ({}) : window.kiosk.layout }
            NumberAnimation { target: curtain; property: "opacity"; to: 0; duration: 600; easing.type: Easing.OutQuad }
        }

        Connections {
            target: window.kiosk

            function onLayoutChanged() {
                layoutSwitch.restart()
            }

            function onScreenOffChanged() {
                layoutSwitch.restart()
            }
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
        enabled: !window.kiosk.publicMode && (window.windowed || window.kiosk.debug)
        onActivated: debugOverlay.visible = !debugOverlay.visible
    }

    Shortcut {
        sequences: [StandardKey.Quit, "Esc"]
        enabled: !window.kiosk.publicMode && (window.windowed || window.kiosk.debug)
        onActivated: Qt.quit()
    }
}
