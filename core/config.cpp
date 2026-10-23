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
    const QString power = display.value(u"power").toString().trimmed().toLower();
    if (power == u"cec")
        settings.power = ScreenPower::Cec;
    else if (power == u"ddc")
        settings.power = ScreenPower::Ddc;
    return settings;
}

QString Config::mediaRoot() const
{
    const QString path = m_json.value(u"media").toObject().value(u"path").toString(QStringLiteral("media"));
    return QDir::cleanPath(QDir(m_baseDir).absoluteFilePath(path));
}

LibrarySettings Config::library() const
{
    const QJsonObject library = m_json.value(u"library").toObject();
    const auto resolve = [&](QStringView key, const QString &fallback) {
        return QDir::cleanPath(QDir(m_baseDir).absoluteFilePath(library.value(key).toString(fallback)));
    };

    LibrarySettings settings;
    settings.database = resolve(u"database", QStringLiteral("library.db"));
    settings.cacheRoot = resolve(u"cache", QStringLiteral("cache"));
    settings.stateDir = resolve(u"state", QStringLiteral("state"));
    settings.trashDir = resolve(u"trash", QStringLiteral("trash"));
    settings.jpegQuality = qBound(50, library.value(u"jpegQuality").toInt(88), 100);

    // "auto" (or nothing) leaves the choice to the display's render tier
    for (const QJsonValue &value : library.value(u"renditions").toArray()) {
        const QString tier = value.toString().trimmed().toLower();
        QString name;
        if (tier == u"1080p" || tier == u"1080" || tier == u"hd")
            name = tierName(RenderTier::Hd1080);
        else if (tier == u"2160p" || tier == u"2160" || tier == u"4k" || tier == u"uhd")
            name = tierName(RenderTier::Uhd2160);
        if (!name.isEmpty() && !settings.renditions.contains(name))
            settings.renditions.append(name);
    }
    return settings;
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

AdminSettings Config::admin() const
{
    const QJsonObject admin = m_json.value(u"admin").toObject();
    AdminSettings settings;
    const QString address = admin.value(u"address").toString().trimmed();
    if (!address.isEmpty())
        settings.address = address;
    const int port = admin.value(u"port").toInt(settings.port);
    if (port > 0 && port <= 65535)
        settings.port = quint16(port);
    settings.enabled = admin.value(u"enabled").toBool(true);
    for (const QJsonValue &host : admin.value(u"allowedHosts").toArray()) {
        if (host.isString() && !host.toString().trimmed().isEmpty())
            settings.allowedHosts.append(host.toString().trimmed().toLower());
    }
    // Uploads are held in memory; more than this does not fit a small Pi
    const int maxUploadMB = admin.value(u"maxUploadMB").toInt(64);
    settings.maxUploadBytes = qint64(qBound(1, maxUploadMB, 128)) * 1024 * 1024;
    return settings;
}

QJsonObject Config::layoutById(const QString &id) const
{
    for (const QJsonValue &value : m_json.value(u"layouts").toArray()) {
        const QJsonObject layout = value.toObject();
        if (!id.isEmpty() && layout.value(u"id").toString() == id)
            return layout;
    }
    return {};
}

bool Config::isPublic() const
{
    return m_json.value(u"profile").toString().trimmed().toLower() == u"public";
}

QJsonArray Config::schedule() const
{
    return m_json.value(u"schedule").toArray();
}

QString Config::locale() const
{
    const QString locale = m_json.value(u"locale").toString().trimmed();
    return locale.isEmpty() ? QStringLiteral("de_DE") : locale;
}

QJsonArray Config::quotes() const
{
    QJsonArray quotes;
    for (const QJsonValue &value : m_json.value(u"quotes").toArray()) {
        QJsonObject quote = value.isString() ? QJsonObject{{QStringLiteral("text"), value.toString()}} : value.toObject();
        if (!quote.value(u"text").toString().trimmed().isEmpty())
            quotes.append(quote);
    }
    return quotes;
}

} // namespace berry
