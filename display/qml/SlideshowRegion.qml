import QtQuick
import BerryKiosk

// Shows the images of one album and cross-fades between two image slots.
// Only the visible slot and the one being loaded hold decoded pixels.
Item {
    id: slideshow

    required property KioskController kiosk
    property var config: ({})
    readonly property size renderSize: kiosk.renderSize

    readonly property int interval: Math.max(1, config.interval ?? 15) * 1000
    readonly property string transitionType: config.transition?.type ?? "fade"
    readonly property int transitionDuration: transitionType === "none" ? 0 : Math.max(0, config.transition?.duration ?? 1200)
    readonly property bool cover: config.fit !== "contain"

    // Decode no larger than the region on screen and never above the render tier
    readonly property size decodeSize: {
        const w = Math.ceil(width * Screen.devicePixelRatio)
        const h = Math.ceil(height * Screen.devicePixelRatio)
        if (w <= 0 || h <= 0)
            return Qt.size(0, 0)
        const scale = Math.min(1, renderSize.width / w, renderSize.height / h)
        return Qt.size(Math.round(w * scale), Math.round(h * scale))
    }
    readonly property bool canRun: decodeSize.width > 0 && media.count > 0

    // Emitted when an image starts to fade in; widgets follow it
    signal shown(string path)

    property bool started: false
    property string pendingPath
    property Image current: null  // slot on screen
    property Image pending: null  // slot being loaded
    property Image previous: null // slot fading out

    clip: true
    onCanRunChanged: if (canRun && !started) advance()

    function advance() {
        const entry = media.next()
        if (!entry.source || !canRun) {
            started = false
            return
        }
        started = true
        pending = current === slotA ? slotB : slotA
        pendingPath = entry.original
        pending.source = "image://media/" + (cover ? "cover" : "contain") + "/" + encodeURIComponent(entry.source)
        if (pending.status === Image.Ready)
            reveal()
    }

    function slotStatusChanged(slot) {
        if (slot !== pending)
            return
        if (slot.status === Image.Ready) {
            reveal()
        } else if (slot.status === Image.Error) {
            slot.source = ""
            pending = null
            retryTimer.start()
        }
    }

    function reveal() {
        previous = current
        current = pending
        pending = null
        current.z = 1
        if (previous)
            previous.z = 0
        shown(pendingPath)

        if (transitionDuration > 0) {
            fadeIn.target = current
            fadeIn.restart()
        } else {
            current.opacity = 1
            finishTransition()
        }
    }

    function finishTransition() {
        if (previous) {
            previous.opacity = 0
            previous.source = "" // frees the decoded image
            previous = null
        }
        if (!kiosk.paused)
            timer.restart()
    }

    readonly property bool idle: started && pending === null && !fadeIn.running

    // Remote control from the admin service
    Connections {
        target: slideshow.kiosk

        function onPausedChanged() {
            if (slideshow.kiosk.paused)
                timer.stop()
            else if (slideshow.idle)
                timer.restart()
        }

        function onNextRequested() {
            if (slideshow.idle) {
                timer.stop()
                slideshow.advance()
            }
        }
    }

    MediaSource {
        id: media

        root: slideshow.kiosk.mediaRoot
        database: slideshow.kiosk.libraryDatabase
        cacheRoot: slideshow.kiosk.cacheRoot
        stateDir: slideshow.kiosk.stateDir
        tier: slideshow.kiosk.renderTier
        album: slideshow.config.album ?? ""
        order: slideshow.config.order ?? "sequential"
    }

    Image {
        id: slotA

        anchors.fill: parent
        asynchronous: true
        cache: false
        smooth: true
        opacity: 0
        fillMode: slideshow.cover ? Image.PreserveAspectCrop : Image.PreserveAspectFit
        sourceSize: slideshow.decodeSize
        onStatusChanged: slideshow.slotStatusChanged(slotA)
    }

    Image {
        id: slotB

        anchors.fill: parent
        asynchronous: true
        cache: false
        smooth: true
        opacity: 0
        fillMode: slideshow.cover ? Image.PreserveAspectCrop : Image.PreserveAspectFit
        sourceSize: slideshow.decodeSize
        onStatusChanged: slideshow.slotStatusChanged(slotB)
    }

    NumberAnimation {
        id: fadeIn

        property: "opacity"
        from: 0
        to: 1
        duration: slideshow.transitionDuration
        easing.type: Easing.InOutQuad
        onFinished: slideshow.finishTransition()
    }

    Timer {
        id: timer

        interval: slideshow.interval
        onTriggered: slideshow.advance()
    }

    // Skips unreadable files without spinning when every file is broken
    Timer {
        id: retryTimer

        interval: 2000
        onTriggered: slideshow.advance()
    }

    Text {
        anchors.centerIn: parent
        width: parent.width * 0.8
        visible: media.count === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        color: "#888888"
        font.pixelSize: Math.max(14, Math.round(slideshow.height / 30))
        text: qsTr("Keine Bilder in\n%1").arg(media.directory || slideshow.kiosk.mediaRoot)
    }
}
