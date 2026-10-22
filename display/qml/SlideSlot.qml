import QtQuick

// One picture of a slideshow: the photo and, with fit "contain", a blurred
// copy of it filling the empty bars. Transitions move or scale this item; the
// Ken Burns drift moves the photo inside it.
Item {
    id: slot

    property string file // rendition or original to show, "" = empty
    property bool cover: true
    property bool backdrop: true
    property size decodeSize
    readonly property int status: photo.status

    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    opacity: 0

    // Back to the resting state before the slot is used for the next picture
    function reset() {
        kenBurns.stop()
        x = 0
        y = 0
        scale = 1
        photo.x = 0
        photo.y = 0
        photo.scale = 1
    }

    // Slow zoom and pan over the whole time the picture is on screen. Only as
    // much movement as the zoom leaves room for, so no edge ever shows.
    function startKenBurns(durationMs) {
        const zoomIn = Math.random() < 0.5
        const startScale = zoomIn ? 1.0 : 1.14
        const endScale = zoomIn ? 1.14 : 1.0
        const room = (scale) => ({ x: (scale - 1) * width / 2, y: (scale - 1) * height / 2 })
        const pick = (scale) => {
            const r = room(scale)
            return { x: (Math.random() * 2 - 1) * r.x, y: (Math.random() * 2 - 1) * r.y }
        }
        const from = pick(startScale)
        const to = pick(endScale)
        kbScale.from = startScale
        kbScale.to = endScale
        kbX.from = from.x
        kbX.to = to.x
        kbY.from = from.y
        kbY.to = to.y
        kenBurns.duration = Math.max(1000, durationMs)
        kenBurns.restart()
    }

    Image {
        id: blurred

        anchors.fill: parent
        visible: !slot.cover && slot.backdrop && slot.file !== ""
        // A tiny, blurred decode that the GPU scales up: costs next to nothing
        source: visible ? "image://media/backdrop/" + encodeURIComponent(slot.file) : ""
        sourceSize: Qt.size(64, 64)
        asynchronous: true
        cache: false
        smooth: true
        fillMode: Image.PreserveAspectCrop
    }

    Rectangle {
        anchors.fill: parent
        visible: blurred.visible
        color: "black"
        opacity: 0.35
    }

    Image {
        id: photo

        width: slot.width
        height: slot.height
        transformOrigin: Item.Center
        source: slot.file === "" ? "" : "image://media/" + (slot.cover ? "cover" : "contain") + "/" + encodeURIComponent(slot.file)
        sourceSize: slot.decodeSize
        asynchronous: true
        cache: false
        smooth: true
        fillMode: slot.cover ? Image.PreserveAspectCrop : Image.PreserveAspectFit
    }

    ParallelAnimation {
        id: kenBurns

        property int duration: 20000

        NumberAnimation { id: kbScale; target: photo; property: "scale"; duration: kenBurns.duration; easing.type: Easing.InOutSine }
        NumberAnimation { id: kbX; target: photo; property: "x"; duration: kenBurns.duration; easing.type: Easing.InOutSine }
        NumberAnimation { id: kbY; target: photo; property: "y"; duration: kenBurns.duration; easing.type: Easing.InOutSine }
    }
}
