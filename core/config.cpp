#include "config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace berry {

namespace {

QJsonObject defaultLayout()
{
    return QJsonObject{
        {QStringLiteral("id"), QStringLiteral("default")},
        {QStringLiteral("grid"), QJsonObject{{QStringLiteral("columns"), 12}, {QStringLiteral("rows"), 12}}},
        {QStringLiteral("regions"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("main")},
             {QStringLiteral("type"), QStringLiteral("slideshow")},
             {QStringLiteral("x"), 0},
             {QStringLiteral("y"), 0},
             {QStringLiteral("w"), 12},
             {QStringLiteral("h"), 12},
         }}},
    };
}

} // namespace

Config Config::fromJson(const QJsonObject &json, const QString &baseDir)
{
    Config config;
    config.m_json = json;
    config.m_baseDir = baseDir;
    return config;
}

Config Config::load(const QString &path, QString *error)
{
    const QString baseDir = QFileInfo(path).absolutePath();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return fromJson({}, baseDir);
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!doc.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                ? QStringLiteral("%1: %2 (Zeichen %3)").arg(path, parseError.errorString()).arg(parseError.offset)
                : QStringLiteral("%1: kein JSON-Objekt").arg(path);
        }
        return fromJson({}, baseDir);
    }

    if (error)
        error->clear();
    return fromJson(doc.object(), baseDir);
}

DisplaySettings Config::display() const
{
    const QJsonObject display = m_json.value(u"display").toObject();

    DisplaySettings settings;
    settings.maxRender = maxRenderFromString(display.value(u"maxRender").toString());
    const int rotation = display.value(u"rotation").toInt();
    settings.rotation = rotation % 90 == 0 ? (rotation % 360 + 360) % 360 : 0;
    return settings;
}

QString Config::mediaRoot() const
{
    const QString path = m_json.value(u"media").toObject().value(u"path").toString(QStringLiteral("media"));
    return QDir::cleanPath(QDir(m_baseDir).absoluteFilePath(path));
}

QJsonObject Config::activeLayout() const
{
    const QJsonArray layouts = m_json.value(u"layouts").toArray();
    const QString activeId = m_json.value(u"activeLayout").toString();

    if (!activeId.isEmpty()) {
        for (const QJsonValue &value : layouts) {
            const QJsonObject layout = value.toObject();
            if (layout.value(u"id").toString() == activeId)
                return layout;
        }
    }
    if (!layouts.isEmpty() && layouts.first().isObject())
        return layouts.first().toObject();
    return defaultLayout();
}

} // namespace berry
