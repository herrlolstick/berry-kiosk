#include "mediaimageprovider.h"

#include "rendertier.h"

#include <QImageReader>
#include <QUrl>

MediaImageProvider::MediaImageProvider(int maxTextureSize)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_maxTextureSize(maxTextureSize)
{
}

QImage MediaImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const qsizetype slash = id.indexOf(u'/');
    const bool cover = id.left(slash) != u"contain";
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

    const QImage image = reader.read();
    if (image.isNull())
        qWarning().noquote() << "Bild nicht lesbar:" << path << '-' << reader.errorString();
    if (size)
        *size = original.isValid() ? original : image.size();
    return image;
}
