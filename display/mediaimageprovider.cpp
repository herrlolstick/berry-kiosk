#include "mediaimageprovider.h"

#include "rendertier.h"

#include <QImageReader>
#include <QUrl>

namespace {

// Cheap blur for tiny images: a few box blur passes approximate a Gaussian
QImage blurred(const QImage &source, int radius, int passes)
{
    QImage image = source.convertToFormat(QImage::Format_RGB32);
    const int w = image.width();
    const int h = image.height();
    QImage buffer(image.size(), QImage::Format_RGB32);
    const auto pass = [radius](const QImage &from, QImage &to, bool horizontal) {
        const int w = from.width();
        const int h = from.height();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int d = -radius; d <= radius; ++d) {
                    const int sx = horizontal ? qBound(0, x + d, w - 1) : x;
                    const int sy = horizontal ? y : qBound(0, y + d, h - 1);
                    const QRgb pixel = reinterpret_cast<const QRgb *>(from.constScanLine(sy))[sx];
                    r += qRed(pixel);
                    g += qGreen(pixel);
                    b += qBlue(pixel);
                    ++n;
                }
                reinterpret_cast<QRgb *>(to.scanLine(y))[x] = qRgb(r / n, g / n, b / n);
            }
        }
    };
    if (w == 0 || h == 0)
        return image;
    for (int i = 0; i < passes; ++i) {
        pass(image, buffer, true);
        pass(buffer, image, false);
    }
    return image;
}

} // namespace

MediaImageProvider::MediaImageProvider(int maxTextureSize)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_maxTextureSize(maxTextureSize)
{
}

QImage MediaImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    // id: "<cover|contain|backdrop>/<percent-encoded path>"
    const qsizetype slash = id.indexOf(u'/');
    const QString fit = id.left(slash);
    const bool backdrop = fit == u"backdrop";
    const bool cover = fit != u"contain";
    const QString path = QUrl::fromPercentEncoding(id.mid(slash + 1).toUtf8());

    QImageReader reader(path);
    reader.setAutoTransform(true);

    // The reader scales before applying the EXIF rotation, so work in stored orientation
    const bool rotated = reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
    QSize original = reader.size();
    if (rotated)
        original.transpose();

    QSize target = berry::fitDecodeSize(original, requestedSize, cover, m_maxTextureSize);
    if (target.isValid() && target != original) {
        if (rotated)
            target.transpose();
        reader.setScaledSize(target);
    }

    QImage image = reader.read();
    if (image.isNull())
        qWarning().noquote() << "Bild nicht lesbar:" << path << '-' << reader.errorString();
    else if (backdrop)
        image = blurred(image, 2, 3);
    if (size)
        *size = original.isValid() ? original : image.size();
    return image;
}
