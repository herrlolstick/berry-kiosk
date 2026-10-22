import QtQuick
import BerryKiosk

// Title and details of the image a slideshow shows, from EXIF/IPTC/XMP and the
// sidecar file. Hides itself for images without any of the chosen fields.
//
// config: follows (region id), fields (["title", "location", "date"]),
//         dateFormat ("d. MMMM yyyy")
// fields: title, location, date, description, camera, lens, exposure
WidgetPanel {
    id: info

    property string locale: "de_DE"
    property string imagePath
    property var lines: [] // [{ text, size }], size relative to the title

    readonly property var fields: config.fields ?? ["title", "location", "date"]
    readonly property string dateFormat: config.dateFormat ?? "d. MMMM yyyy"
    // Font size unit so that all lines fill the height; long lines shrink to the width
    readonly property real unit: {
        const total = lines.reduce((sum, line) => sum + line.size * 1.25, 0)
        return total > 0 ? column.parent.height / total : 0
    }

    shown: lines.length > 0
    contentWidth: {
        let widest = 0
        for (let i = 0; i < lineRepeater.count; ++i)
            widest = Math.max(widest, lineRepeater.itemAt(i)?.implicitWidth ?? 0)
        return widest
    }

    function buildLines(metadata) {
        const values = {
            title: metadata.title ?? "",
            location: metadata.location ?? "",
            date: metadata.taken ? metadata.taken.toLocaleDateString(Qt.locale(locale), dateFormat) : "",
            description: metadata.description ?? "",
            camera: metadata.camera ?? "",
            lens: metadata.lens ?? "",
            exposure: metadata.exposure ?? ""
        }
        // Fields sharing a line, in display order
        const groups = [["title"], ["location", "date"], ["description"], ["camera", "lens"], ["exposure"]]
        const result = []
        for (const group of groups) {
            const text = group.filter(field => fields.includes(field) && values[field] !== "")
                              .map(field => values[field])
                              .join("  ·  ")
            if (text !== "")
                result.push({ text: text, size: group[0] === "title" ? 1 : 0.6 })
        }
        return result
    }

    MetadataReader {
        path: info.imagePath
        locale: info.locale
        onLoaded: {
            const next = info.buildLines(metadata)
            if (JSON.stringify(next) !== JSON.stringify(info.lines))
                info.crossfade(() => info.lines = next)
        }
    }

    Column {
        id: column

        anchors.verticalCenter: parent.verticalCenter
        width: parent.width

        Repeater {
            id: lineRepeater

            model: info.lines

            Text {
                required property var modelData

                width: column.width
                text: modelData.text
                color: info.textColor
                style: info.textStyle
                styleColor: info.textStyleColor
                font.family: info.fontFamily
                font.weight: modelData.size >= 1 ? Font.DemiBold : Font.Normal
                font.pixelSize: Math.max(8, Math.round(info.unit * modelData.size))
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: 8
                horizontalAlignment: info.horizontalAlignment
                textFormat: Text.PlainText
            }
        }
    }
}
