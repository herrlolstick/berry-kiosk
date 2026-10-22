import QtQuick
import BerryKiosk

// Places the regions of the active layout on a grid of columns × rows cells.
// Positions are in cells, so the same layout works at 1080p and 4K.
Item {
    id: view

    required property KioskController kiosk

    readonly property var layout: kiosk.layout
    readonly property int columns: Math.max(1, layout.grid?.columns ?? 12)
    readonly property int rows: Math.max(1, layout.grid?.rows ?? 12)

    function columnX(column) {
        return Math.round(column * width / columns)
    }

    function rowY(row) {
        return Math.round(row * height / rows)
    }

    Repeater {
        model: view.layout.regions ?? []

        delegate: Item {
            id: region

            required property var modelData

            readonly property int column: modelData.x ?? 0
            readonly property int row: modelData.y ?? 0

            // Right and bottom edges are rounded on their own so neighbours meet without gaps
            x: view.columnX(column)
            y: view.rowY(row)
            width: view.columnX(column + (modelData.w ?? view.columns)) - x
            height: view.rowY(row + (modelData.h ?? view.rows)) - y
            z: modelData.z ?? 0

            Loader {
                anchors.fill: parent
                active: region.modelData.type === "slideshow"
                sourceComponent: SlideshowRegion {
                    config: region.modelData
                    mediaRoot: view.kiosk.mediaRoot
                    renderSize: view.kiosk.renderSize
                }
            }

            Component.onCompleted: {
                if (modelData.type !== "slideshow")
                    console.warn(`Region "${modelData.id}": Typ "${modelData.type}" wird noch nicht unterstützt`)
            }
        }
    }
}
