import QtQuick

// Time and, optionally, the date. Text scales to the region.
//
// config: format ("HH:mm"), showDate (true), dateFormat ("dddd, d. MMMM")
WidgetPanel {
    id: clock

    property string locale: "de_DE"
    property date now: new Date()

    readonly property string timeFormat: config.format ?? "HH:mm"
    readonly property bool showDate: config.showDate ?? true
    readonly property string dateFormat: config.dateFormat ?? "dddd, d. MMMM"
    readonly property bool showSeconds: timeFormat.indexOf("s") >= 0

    defaultAlign: "right"

    // Wakes up exactly at the next minute (or second), not every second
    function tick() {
        now = new Date()
        ticker.interval = showSeconds
            ? 1000 - now.getMilliseconds()
            : 60000 - now.getSeconds() * 1000 - now.getMilliseconds()
        ticker.restart()
    }

    Component.onCompleted: tick()

    Timer {
        id: ticker

        onTriggered: clock.tick()
    }

    Text {
        id: time

        width: parent.width
        height: clock.showDate ? Math.round(parent.height * 0.68) : parent.height
        text: clock.now.toLocaleTimeString(Qt.locale(clock.locale), clock.timeFormat)
        color: clock.textColor
        style: clock.textStyle
        styleColor: clock.textStyleColor
        font.family: clock.fontFamily
        font.weight: Font.Light
        font.pixelSize: Math.max(8, height)
        fontSizeMode: Text.Fit
        minimumPixelSize: 8
        horizontalAlignment: clock.horizontalAlignment
        verticalAlignment: Text.AlignVCenter
        textFormat: Text.PlainText // the format comes from config.json, never markup
    }

    Text {
        anchors.top: time.bottom
        width: parent.width
        height: parent.height - time.height
        visible: clock.showDate
        text: clock.now.toLocaleDateString(Qt.locale(clock.locale), clock.dateFormat)
        color: clock.textColor
        style: clock.textStyle
        styleColor: clock.textStyleColor
        font.family: clock.fontFamily
        font.pixelSize: Math.max(8, height)
        fontSizeMode: Text.Fit
        minimumPixelSize: 8
        horizontalAlignment: clock.horizontalAlignment
        verticalAlignment: Text.AlignVCenter
        textFormat: Text.PlainText // the format comes from config.json, never markup
    }
}
