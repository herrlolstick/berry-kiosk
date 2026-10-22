#pragma once

#include <QSize>
#include <QString>

namespace berry {

// Rendition kinds: the render tiers "1080p" and "2160p", plus "thumb" for the admin UI.
bool isRenditionKind(const QString &kind);

// Size of a rendition for an upright image. Tier renditions cover a square of
// the tier's long side, so the same file fills landscape and portrait screens,
// and stay within the texture limit of the boards that use the tier (2048 px
// for 1080p, 4096 px for 2160p). Thumbnails fit into 512 × 512. Never upscales.
QSize renditionSize(QSize original, const QString &kind);

// Image size after applying the EXIF orientation, read from the header only
QSize uprightImageSize(const QString &path);

// Writes an upright JPEG rendition of source to target, atomically. Transparent
// areas become black, like on screen. size receives the rendition's size.
bool createRendition(const QString &source, const QString &target, const QString &kind, int quality,
                     QSize *size = nullptr, QString *error = nullptr);

} // namespace berry
