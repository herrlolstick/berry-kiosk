import QtQuick
import BerryKiosk

// Places the regions of the active layout on a grid of columns × rows cells.
// Positions are in cells, so the same layout works at 1080p and 4K.
Item {
    id: view

    required property KioskController kiosk

    readonly property var layout: kiosk.layout
    readonly property var regions: layout.regions ?? []
    readonly property int columns: Math.max(1, layout.grid?.columns ?? 12)
    readonly property int rows: Math.max(1, layout.grid?.rows ?? 12)

    // Image each slideshow currently shows, by region id; widgets follow these
    property var currentImages: ({})
    // Widgets without "follows" follow the first slideshow
    readonly property string firstSlideshowId: {
        const index = regions.findIndex(region => region.type === "slideshow")
        return index >= 0 ? regionId(regions[index], index) : ""
    }

    onLayoutChanged: currentImages = {}

    function regionId(region, index) {
        return region.id ?? `region-${index}`
    }

    function setCurrentImage(id, path) {
        const images = Object.assign({}, currentImages)
        images[id] = path
        currentImages = images
    }

    function columnX(column) {
        return Math.round(column * width / columns)
    }

    function rowY(row) {
        return Math.round(row * height / rows)
    }

    Repeater {
        model: view.regions

        delegate: Item {
            id: region

            required property var modelData
            required property int index

            readonly property string regionId: view.regionId(modelData, index)
            readonly property string followedId: modelData.follows ?? view.firstSlideshowId
            readonly property string followedImage: view.currentImages[followedId] ?? ""
            readonly property int column: modelData.x ?? 0
            readonly property int row: modelData.y ?? 0

            // Right and bottom edges are rounded on their own so neighbours meet without gaps
            x: view.columnX(column)
            y: view.rowY(row)
            width: view.columnX(column + (modelData.w ?? view.columns)) - x
            height: view.rowY(row + (modelData.h ?? view.rows)) - y
            z: modelData.z ?? (modelData.type === "slideshow" ? 0 : 1)

            Loader {
                anchors.fill: parent
                sourceComponent: {
                    switch (region.modelData.type) {
                    case "slideshow": return slideshowComponent
                    case "clock": return clockComponent
                    case "imageInfo": return imageInfoComponent
                    case "quote": return quoteComponent
                    }
                    return null
                }
            }

            Component {
                id: slideshowComponent

                SlideshowRegion {
                    config: region.modelData
                    mediaRoot: view.kiosk.mediaRoot
                    renderSize: view.kiosk.renderSize
                    onShown: path => view.setCurrentImage(region.regionId, path)
                }
            }

            Component {
                id: clockComponent

                ClockWidget {
                    config: region.modelData
                    locale: view.kiosk.locale
                }
            }

            Component {
                id: imageInfoComponent

                ImageInfoWidget {
                    config: region.modelData
                    locale: view.kiosk.locale
                    imagePath: region.followedImage
                }
            }

            Component {
                id: quoteComponent

                QuoteWidget {
                    config: region.modelData
                    locale: view.kiosk.locale
                    imagePath: region.followedImage
                    followsSlideshow: region.followedId !== ""
                    quotes: view.kiosk.quotes
                }
            }

            Component.onCompleted: {
                if (!["slideshow", "clock", "imageInfo", "quote"].includes(modelData.type))
                    console.warn(`Region "${regionId}": Typ "${modelData.type}" wird nicht unterstützt`)
            }
        }
    }
}
