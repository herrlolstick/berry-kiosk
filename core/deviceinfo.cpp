#include "deviceinfo.h"

#include <QFile>
#include <QRegularExpression>

namespace berry {

DeviceInfo DeviceInfo::fromModelString(const QString &model)
{
    DeviceInfo info;
    info.model = model.trimmed();
    info.isRaspberryPi = info.model.startsWith(u"Raspberry Pi");

    // Pi 4 and newer use VideoCore VI/VII with 4096 px textures and 4K output
    static const QRegularExpression modernGpu(
        QStringLiteral(R"(\bPi (4|5|400|500)\b|Compute Module (4S?|5)\b)"));
    info.legacyGpu = info.isRaspberryPi && !modernGpu.match(info.model).hasMatch();
    return info;
}

DeviceInfo DeviceInfo::detect()
{
    QFile file(QStringLiteral("/proc/device-tree/model"));
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QByteArray raw = file.readAll();
    // The device tree string is NUL terminated
    const qsizetype nul = raw.indexOf('\0');
    if (nul >= 0)
        raw.truncate(nul);
    return fromModelString(QString::fromUtf8(raw));
}

} // namespace berry
