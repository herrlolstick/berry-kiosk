#pragma once

#include "rendertier.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace berry {

struct DisplaySettings
{
    MaxRender maxRender = MaxRender::Uhd2160;
    int rotation = 0; // 0, 90, 180 or 270
};

struct LibrarySettings
{
    QString database;       // absolute
    QString cacheRoot;      // absolute; renditions live in <cacheRoot>/<kind>/
    QString stateDir;       // absolute
    QString trashDir;       // absolute; deleted pictures are moved here
    QStringList renditions; // tiers to prepare ("1080p", "2160p"); empty = the one the display uses
    int jpegQuality = 88;
};

struct AdminSettings
{
    QString address = QStringLiteral("0.0.0.0"); // "0.0.0.0" = every interface, "127.0.0.1" = this device only
    quint16 port = 8080;
    qint64 maxUploadBytes = 64 * 1024 * 1024;
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
    LibrarySettings library() const; // paths resolve like mediaRoot
    AdminSettings admin() const;
    QJsonObject activeLayout() const; // falls back to one full-screen slideshow
    QString locale() const;           // for dates and numbers, e.g. "de_DE"
    QJsonArray quotes() const;        // [{ "text", "author" }], plain strings are accepted too

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
