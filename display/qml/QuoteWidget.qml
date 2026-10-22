import QtQuick
import BerryKiosk

// A quote that matches the picture: from the image's sidecar file, otherwise
// the next one from the global list in config.json.
//
// config: follows (region id), source ("image-or-global", "image", "global"),
//         interval (seconds between global quotes when no slideshow is followed)
WidgetPanel {
    id: quoteWidget

    property string locale: "de_DE"
    property string imagePath
    property bool followsSlideshow: false
    property var quotes: []
    property int nextGlobal: 0
    property string quoteText
    property string author

    readonly property string source: config.source ?? "image-or-global"
    readonly property bool german: locale.startsWith("de")

    defaultAlign: "center"
    shown: quoteText !== ""

    Component.onCompleted: nextGlobal = Math.floor(Math.random() * Math.max(1, quotes.length))

    function pick(metadata) {
        let text = ""
        let by = ""
        if (source !== "global" && metadata.quote) {
            text = metadata.quote
            by = metadata.quoteAuthor ?? ""
        } else if (source !== "image" && quotes.length > 0) {
            const quote = quotes[nextGlobal % quotes.length]
            nextGlobal = (nextGlobal + 1) % quotes.length
            text = quote.text ?? ""
            by = quote.author ?? ""
        }
        if (text === quoteText && by === author)
            return
        crossfade(() => {
            quoteWidget.quoteText = text
            quoteWidget.author = by
        })
    }

    // A new picture brings a new quote
    MetadataReader {
        path: quoteWidget.imagePath
        locale: quoteWidget.locale
        onLoaded: if (quoteWidget.imagePath !== "") quoteWidget.pick(metadata)
    }

    // Without a slideshow to follow, global quotes change on a timer
    Timer {
        running: !quoteWidget.followsSlideshow && quoteWidget.source !== "image"
        interval: Math.max(5, quoteWidget.config.interval ?? 60) * 1000
        repeat: true
        triggeredOnStart: true
        onTriggered: quoteWidget.pick({})
    }

    Text {
        id: quoteLabel

        width: parent.width
        height: quoteWidget.author !== "" ? Math.round(parent.height * 0.76) : parent.height
        text: quoteWidget.german ? `„${quoteWidget.quoteText}“` : `“${quoteWidget.quoteText}”`
        color: quoteWidget.textColor
        style: quoteWidget.textStyle
        styleColor: quoteWidget.textStyleColor
        font.family: quoteWidget.fontFamily
        font.italic: quoteWidget.style.italic ?? true
        font.pixelSize: Math.max(8, height)
        fontSizeMode: Text.Fit
        minimumPixelSize: 8
        wrapMode: Text.WordWrap
        horizontalAlignment: quoteWidget.horizontalAlignment
        verticalAlignment: Text.AlignVCenter
        textFormat: Text.PlainText
    }

    Text {
        anchors.top: quoteLabel.bottom
        width: parent.width
        height: parent.height - quoteLabel.height
        visible: quoteWidget.author !== ""
        text: "– " + quoteWidget.author
        color: quoteWidget.textColor
        style: quoteWidget.textStyle
        styleColor: quoteWidget.textStyleColor
        font.family: quoteWidget.fontFamily
        font.pixelSize: Math.max(8, Math.round(height * 0.8))
        fontSizeMode: Text.Fit
        minimumPixelSize: 8
        horizontalAlignment: quoteWidget.horizontalAlignment === Text.AlignLeft ? Text.AlignLeft : Text.AlignRight
        verticalAlignment: Text.AlignVCenter
        textFormat: Text.PlainText
    }
}
