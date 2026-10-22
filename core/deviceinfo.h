#pragma once

#include <QString>

namespace berry {

struct DeviceInfo
{
    QString model; // e.g. "Raspberry Pi 4 Model B Rev 1.5", empty when unknown
    bool isRaspberryPi = false;
    // VideoCore IV boards (Pi Zero/1/2/3): textures up to 2048 px and no
    // usable 4K output, so they always run in 1080p.
    bool legacyGpu = false;

    static DeviceInfo detect();
    static DeviceInfo fromModelString(const QString &model);
};

} // namespace berry
