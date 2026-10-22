#include "rendertier.h"

#include <QtGlobal>

namespace berry {

MaxRender maxRenderFromString(const QString &value)
{
    const QString v = value.trimmed().toLower();
    if (v == u"1080p" || v == u"1080" || v == u"hd")
        return MaxRender::Hd1080;
    return MaxRender::Uhd2160; // "auto", "2160p", "4k" and anything unknown
}

QString tierName(RenderTier tier)
{
    return tier == RenderTier::Uhd2160 ? QStringLiteral("2160p") : QStringLiteral("1080p");
}

QSize tierSize(RenderTier tier)
{
    return tier == RenderTier::Uhd2160 ? QSize(3840, 2160) : QSize(1920, 1080);
}

RenderDecision decideRenderTier(QSize screenSize, int maxTextureSize, MaxRender maxRender)
{
    RenderDecision decision;
    const int longSide = qMax(screenSize.width(), screenSize.height());
    const int uhdLongSide = tierSize(RenderTier::Uhd2160).width();
    const int hdLongSide = tierSize(RenderTier::Hd1080).width();

    if (maxRender == MaxRender::Hd1080) {
        decision.reason = QStringLiteral("auf 1080p begrenzt (Einstellung)");
    } else if (screenSize.isEmpty()) {
        decision.reason = QStringLiteral("Bildschirmgröße unbekannt");
    } else if (longSide <= hdLongSide) {
        decision.reason = QStringLiteral("Bildschirm %1×%2").arg(screenSize.width()).arg(screenSize.height());
    } else if (maxTextureSize > 0 && maxTextureSize < uhdLongSide) {
        decision.reason = QStringLiteral("GPU kann nur Texturen bis %1 px").arg(maxTextureSize);
    } else {
        // Everything above Full HD (QHD monitors included) gets 4K material,
        // decoded down to the screen size.
        decision.tier = RenderTier::Uhd2160;
        decision.reason = QStringLiteral("Bildschirm und GPU können 4K");
    }

    QSize size = tierSize(decision.tier);
    if (screenSize.height() > screenSize.width())
        size.transpose();
    if (!screenSize.isEmpty())
        size = size.boundedTo(screenSize);
    decision.renderSize = size;
    return decision;
}

QSize fitDecodeSize(QSize original, QSize box, bool cover, int maxTextureSize)
{
    if (original.isEmpty())
        return original;

    double scale = 1.0;
    if (!box.isEmpty()) {
        const double sx = double(box.width()) / original.width();
        const double sy = double(box.height()) / original.height();
        scale = qMin(1.0, cover ? qMax(sx, sy) : qMin(sx, sy));
    }
    if (maxTextureSize > 0) {
        const int longSide = qMax(original.width(), original.height());
        scale = qMin(scale, double(maxTextureSize) / longSide);
    }
    return QSize(qMax(1, qRound(original.width() * scale)),
                 qMax(1, qRound(original.height() * scale)));
}

} // namespace berry
