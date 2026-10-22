#include "renditions.h"

#include "rendertier.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>

namespace berry {

namespace {

bool rotatedBy90(const QImageReader &reader)
{
    return reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
}

} // namespace

bool isRenditionKind(const QString &kind)
{
    return kind == u"1080p" || kind == u"2160p" || kind == u"thumb";
}

QSize renditionSize(QSize original, const QString &kind)
{
    if (kind == u"thumb")
        return fitDecodeSize(original, QSize(512, 512), false, 0);

    const RenderTier tier = kind == u"2160p" ? RenderTier::Uhd2160 : RenderTier::Hd1080;
    const int longSide = tierSize(tier).width();
    const int textureLimit = tier == RenderTier::Uhd2160 ? 4096 : 2048;
    return fitDecodeSize(original, QSize(longSide, longSide), true, textureLimit);
}

QSize uprightImageSize(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (rotatedBy90(reader))
        size.transpose();
    return size.isValid() ? size : QSize();
}

bool createRendition(const QString &source, const QString &target, const QString &kind, int quality,
                     QSize *size, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QImageReader reader(source);
    reader.setAutoTransform(true);
    const QSize stored = reader.size();
    if (stored.isEmpty())
        return fail(reader.errorString());

    const bool rotated = rotatedBy90(reader);
    const QSize upright = rotated ? stored.transposed() : stored;
    const QSize targetSize = renditionSize(upright, kind);

    // The reader scales before rotating; JPEGs are scaled while decoding,
    // which keeps memory low even for very large originals.
    const QSize scaled = rotated ? targetSize.transposed() : targetSize;
    if (scaled != stored)
        reader.setScaledSize(scaled);

    QImage image = reader.read();
    if (image.isNull())
        return fail(reader.errorString());
    if (image.size() != targetSize)
        image = image.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    if (image.hasAlphaChannel()) {
        QImage flat(image.size(), QImage::Format_RGB32);
        flat.fill(Qt::black);
        QPainter painter(&flat);
        painter.drawImage(0, 0, image);
        painter.end();
        image = flat;
    }

    const QString dir = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("%1: Ordner nicht anlegbar").arg(dir));

    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly))
        return fail(file.errorString());
    QImageWriter writer(&file, "jpeg");
    writer.setQuality(quality);
    writer.setOptimizedWrite(true);
    if (!writer.write(image)) {
        file.cancelWriting();
        return fail(writer.errorString());
    }
    if (!file.commit())
        return fail(file.errorString());

    if (size)
        *size = image.size();
    return true;
}

} // namespace berry
