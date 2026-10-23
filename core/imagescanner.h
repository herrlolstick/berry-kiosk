#pragma once

#include <QString>
#include <QStringList>

namespace berry {

bool isSupportedImage(const QString &fileName);

// All supported images below directory as absolute paths, sorted. Symbolic
// links inside it are ignored, so nothing outside the folder is reached. Hidden
// entries (".thumbnails", macOS "._" files) and NAS metadata folders such as
// Synology's "@eaDir" are skipped.
QStringList scanImages(const QString &directory);

} // namespace berry
