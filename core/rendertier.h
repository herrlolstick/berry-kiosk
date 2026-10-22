#pragma once

#include <QSize>
#include <QString>

namespace berry {

// Resolution class the kiosk renders and prepares images for.
enum class RenderTier { Hd1080, Uhd2160 };

// Upper limit from the configuration ("display.maxRender").
enum class MaxRender { Hd1080, Uhd2160 };

MaxRender maxRenderFromString(const QString &value);
QString tierName(RenderTier tier);
QSize tierSize(RenderTier tier); // landscape

struct RenderDecision
{
    RenderTier tier = RenderTier::Hd1080;
    QSize renderSize; // largest size images are decoded to, oriented like the screen
    QString reason;   // human readable, shown in the debug overlay and admin UI
};

// Picks the render tier from what the screen shows, what the GPU can hold and
// what the configuration allows. screenSize is in physical pixels; a
// maxTextureSize of 0 means unknown.
RenderDecision decideRenderTier(QSize screenSize, int maxTextureSize, MaxRender maxRender);

// Size to decode an image to so it covers (or, with cover = false, fits into)
// box. Never upscales and never exceeds maxTextureSize (0 = no limit) on either
// side, so panoramas cannot produce textures the GPU refuses.
QSize fitDecodeSize(QSize original, QSize box, bool cover, int maxTextureSize);

} // namespace berry
