#pragma once

#include "rendertier.h"

#include <QJsonObject>
#include <QString>

namespace berry {

struct DisplaySettings
{
    MaxRender maxRender = MaxRender::Uhd2160;
    int rotation = 0; // 0, 90, 180 or 270
};

// Read-only view on config.json. Missing values fall back to defaults so a
// partial or broken file still gives a working kiosk.
class Config
{
public:
    static Config fromJson(const QJsonObject &json, const QString &baseDir);
    // On failure error is set and the defaults are returned.
    static Config load(const QString &path, QString *error = nullptr);

    const QJsonObject &json() const { return m_json; }
    DisplaySettings display() const;
    QString mediaRoot() const;      // absolute; relative paths resolve against the config file
    QJsonObject activeLayout() const; // falls back to one full-screen slideshow

    bool operator==(const Config &other) const
    {
        return m_json == other.m_json && m_baseDir == other.m_baseDir;
    }
    bool operator!=(const Config &other) const { return !(*this == other); }

private:
    QJsonObject m_json;
    QString m_baseDir;
};

} // namespace berry
