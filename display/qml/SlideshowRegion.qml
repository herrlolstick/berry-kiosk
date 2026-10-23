import QtQuick
import BerryKiosk

// Shows the images of one album with transitions between two picture slots.
// Only the visible slot and the one being loaded hold decoded pixels.
//
// config.transition.type: fade, slide-left, slide-right, slide-up, slide-down,
// zoom, kenburns (fade plus slow drift), mix (a random simple one each time),
// none. When the board cannot keep up (two slow transitions in a row), the
// region falls back to plain fades.
Item {
    id: slideshow

    required property KioskController kiosk
    property var config: ({})
    readonly property size renderSize: kiosk.renderSize

    readonly property int interval: Math.max(1, config.interval ?? 15) * 1000
    readonly property string transitionType: config.transition?.type ?? "fade"
    readonly property int transitionDuration: transitionType === "none" ? 0 : Math.max(0, config.transition?.duration ?? 1200)
    readonly property bool cover: config.fit !== "contain"
    readonly property var mixTypes: ["fade", "slide-left", "slide-right", "slide-up", "slide-down", "zoom"]

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

    // Emitted when an image starts to show; widgets follow it
    signal shown(string path)

    property bool started: false
    property string pendingPath
    property SlideSlot current: null  // slot on screen
    property SlideSlot pending: null  // slot being loaded
    property SlideSlot previous: null // slot leaving

    property string mode: "fade"   // transition running right now
    property real progress: 0
    property bool reduced: false   // only fades from now on
    property int slowInARow: 0
    property int frames: 0

    readonly property bool idle: started && pending === null && !transition.running

    clip: true
    onCanRunChanged: if (canRun && !started) advance()
    onProgressChanged: applyProgress()

    function advance() {
        const entry = media.next()
        if (!entry.source || !canRun) {
            started = false
            return
        }
        started = true
        pending = current === slotA ? slotB : slotA
        pendingPath = entry.original
        pending.reset()
        pending.file = entry.source
        if (pending.status === Image.Ready)
            reveal()
    }

    function slotStatusChanged(slot) {
        if (slot !== pending)
            return
        if (slot.status === Image.Ready) {
            reveal()
        } else if (slot.status === Image.Error) {
            slot.file = ""
            pending = null
            retryTimer.start()
        }
    }

    function pickMode() {
        let type = transitionType
        if (type === "mix")
            type = mixTypes[Math.floor(Math.random() * mixTypes.length)]
        if (reduced && type !== "none")
            type = "fade"
        // Nothing to slide away from for the very first picture
        if (!previous && type.startsWith("slide"))
            type = "fade"
        return type
    }

    function reveal() {
        previous = current
        current = pending
        pending = null
        current.z = 1
        if (previous)
            previous.z = 0
        shown(pendingPath)

        mode = pickMode()
        current.opacity = mode.startsWith("slide") ? 1 : 0
        if (transitionType === "kenburns" && !reduced)
            current.startKenBurns(interval + 2 * transitionDuration)

        if (transitionDuration > 0 && mode !== "none") {
            frames = 0
            progress = 0
            transition.restart()
        } else {
            progress = 1
            finishTransition()
        }
    }

    function applyProgress() {
        const incoming = current
        const outgoing = previous
        if (!incoming)
            return
        const p = progress
        switch (mode) {
        case "slide-left":
            incoming.x = width * (1 - p)
            if (outgoing) outgoing.x = -width * p
            break
        case "slide-right":
            incoming.x = -width * (1 - p)
            if (outgoing) outgoing.x = width * p
            break
        case "slide-up":
            incoming.y = height * (1 - p)
            if (outgoing) outgoing.y = -height * p
            break
        case "slide-down":
            incoming.y = -height * (1 - p)
            if (outgoing) outgoing.y = height * p
            break
        case "zoom":
            incoming.opacity = p
            incoming.scale = 1.08 - 0.08 * p
            break
        default: // fade, kenburns
            incoming.opacity = p
        }
    }

    // A transition that ran at well under 20 frames per second twice in a
    // row means the GPU is overwhelmed: keep the show smooth with fades.
    function measureTransition() {
        if (reduced || transitionDuration < 400 || mode === "fade")
            return
        const fps = frames / (transitionDuration / 1000)
        slowInARow = fps < 20 ? slowInARow + 1 : 0
        if (slowInARow >= 2) {
            reduced = true
            console.warn(`Diashow "${config.id ?? ""}": Übergänge zu langsam (${fps.toFixed(0)} fps), ab jetzt nur Überblenden`)
        }
    }

    function finishTransition() {
        measureTransition()
        if (current)
            current.opacity = 1
        if (previous) {
            previous.opacity = 0
            previous.file = "" // frees the decoded image
            previous.reset()
            previous = null
        }
        if (!kiosk.paused)
            timer.restart()
    }

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

    // afterAnimating runs on the GUI thread once per rendered frame
    Connections {
        target: slideshow.Window.window
        enabled: transition.running

        function onAfterAnimating() {
            slideshow.frames++
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

    SlideSlot {
        id: slotA

        cover: slideshow.cover
        backdrop: slideshow.config.backdrop ?? true
        decodeSize: slideshow.decodeSize
        onStatusChanged: slideshow.slotStatusChanged(slotA)
    }

    SlideSlot {
        id: slotB

        cover: slideshow.cover
        backdrop: slideshow.config.backdrop ?? true
        decodeSize: slideshow.decodeSize
        onStatusChanged: slideshow.slotStatusChanged(slotB)
    }

    NumberAnimation {
        id: transition

        target: slideshow
        property: "progress"
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
        textFormat: Text.PlainText
        text: qsTr("Keine Bilder in\n%1").arg(media.directory || slideshow.kiosk.mediaRoot)
    }
}
