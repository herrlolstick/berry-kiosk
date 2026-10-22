import QtQuick

// Frame shared by all widgets: optional translucent plate, padding, text style,
// fading in and out, and cross-fading when the content changes.
//
// config.style: background ("dark", "light", "none"), color, font, align
// ("left", "center", "right"), opacity, padding and margin (shares of the
// region's shorter side)
Item {
    id: panel

    property var config: ({})
    // Widgets hide themselves (fading out) when there is nothing to show
    property bool shown: true
    property string defaultAlign: "left"
    // Natural width of the content; the plate shrinks to it. -1 fills the region.
    property real contentWidth: -1
    readonly property int fadeDuration: Math.max(0, config.fadeDuration ?? 800)

    readonly property var style: config.style ?? {}
    readonly property string background: style.background ?? "dark"
    readonly property color textColor: style.color ?? (background === "light" ? "#202020" : "white")
    readonly property string fontFamily: style.font ?? Qt.application.font.family
    readonly property int horizontalAlignment: {
        switch (style.align ?? defaultAlign) {
        case "center": return Text.AlignHCenter
        case "right": return Text.AlignRight
        }
        return Text.AlignLeft
    }
    // Without a plate, a soft shadow keeps text readable on bright photos
    readonly property int textStyle: background === "none" ? Text.Raised : Text.Normal
    readonly property color textStyleColor: "#90000000"

    default property alias content: inner.data
    property var pendingApply: null
    property bool ready: false

    opacity: ready && shown ? (style.opacity ?? 1) : 0
    Component.onCompleted: ready = true

    Behavior on opacity {
        NumberAnimation {
            duration: panel.fadeDuration
            easing.type: Easing.InOutQuad
        }
    }

    // Runs apply() while the content is faded out
    function crossfade(apply) {
        if (!shown || fadeDuration === 0) {
            apply()
            return
        }
        pendingApply = apply
        swapAnimation.restart()
    }

    // Keeps plates off the screen edge and away from neighbouring widgets
    Item {
        id: area

        anchors.fill: parent
        anchors.margins: Math.round(Math.min(panel.width, panel.height) * (panel.style.margin ?? 0.08))
    }

    Item {
        id: frame

        readonly property real padding: Math.round(Math.min(area.width, area.height) * (panel.style.padding ?? 0.12))

        y: area.y
        height: area.height
        width: panel.contentWidth < 0 ? area.width : Math.min(area.width, Math.ceil(panel.contentWidth) + 2 * padding)
        x: {
            switch (panel.horizontalAlignment) {
            case Text.AlignHCenter: return area.x + (area.width - width) / 2
            case Text.AlignRight: return area.x + area.width - width
            }
            return area.x
        }

        Rectangle {
            anchors.fill: parent
            visible: panel.background !== "none"
            radius: Math.min(width, height) * 0.12
            color: panel.style.backgroundColor ?? (panel.background === "light" ? "#b0ffffff" : "#80000000")
        }

        Item {
            id: inner

            anchors.fill: parent
            anchors.margins: frame.padding
        }
    }

    SequentialAnimation {
        id: swapAnimation

        NumberAnimation {
            target: inner
            property: "opacity"
            to: 0
            duration: panel.fadeDuration / 2
            easing.type: Easing.InQuad
        }
        ScriptAction {
            script: {
                const apply = panel.pendingApply
                panel.pendingApply = null
                if (apply)
                    apply()
            }
        }
        NumberAnimation {
            target: inner
            property: "opacity"
            to: 1
            duration: panel.fadeDuration / 2
            easing.type: Easing.OutQuad
        }
    }
}
