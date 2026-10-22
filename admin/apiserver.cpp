#include "apiserver.h"

#include "displaylink.h"
#include "displayprotocol.h"
#include "imagemetadata.h"
#include "library.h"
#include "mediafiles.h"
#include "state.h"

#include <QBuffer>
#include <QCborMap>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHttpHeaders>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTcpServer>
#include <QUrlQuery>

#include <algorithm>

namespace {

using Method = QHttpServerRequest::Method;
using Status = QHttpServerResponse::StatusCode;

constexpr auto kSessionCookie = "berry_session";
constexpr auto kCsrfHeader = "X-Berry-Kiosk";
constexpr qsizetype kMaxJsonBody = 1024 * 1024;
constexpr qsizetype kMaxTextField = 2000;

// Every response leaves through here: no sniffing, no framing, a strict CSP
QHttpServerResponse finish(QHttpServerResponse response, bool cacheable = false)
{
    QHttpHeaders headers = response.headers();
    headers.append("X-Content-Type-Options", "nosniff");
    headers.append("X-Frame-Options", "DENY");
    headers.append("Referrer-Policy", "same-origin");
    headers.append("Content-Security-Policy",
                   "default-src 'self'; img-src 'self' blob: data:; style-src 'self'; script-src 'self'; "
                   "frame-ancestors 'none'");
    headers.append("Cache-Control", cacheable ? "private, max-age=300" : "no-store");
    response.setHeaders(std::move(headers));
    return response;
}

QHttpServerResponse json(const QJsonObject &object, Status status = Status::Ok)
{
    return finish(QHttpServerResponse(object, status));
}

QHttpServerResponse json(const QJsonArray &array, Status status = Status::Ok)
{
    return finish(QHttpServerResponse(array, status));
}

QHttpServerResponse fail(Status status, const QString &message)
{
    return json(QJsonObject{{QStringLiteral("error"), message}}, status);
}

QHttpServerResponse empty(Status status = Status::NoContent)
{
    return finish(QHttpServerResponse(status));
}

QByteArray contentType(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"jpg" || suffix == u"jpeg")
        return "image/jpeg";
    if (suffix == u"png")
        return "image/png";
    if (suffix == u"webp")
        return "image/webp";
    if (suffix == u"html")
        return "text/html; charset=utf-8";
    if (suffix == u"js")
        return "text/javascript; charset=utf-8";
    if (suffix == u"css")
        return "text/css; charset=utf-8";
    if (suffix == u"svg")
        return "image/svg+xml";
    return "application/octet-stream";
}

QHttpServerResponse serveFile(const QString &path, bool cacheable)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(Status::NotFound, QStringLiteral("Datei nicht gefunden"));
    return finish(QHttpServerResponse(contentType(path), file.readAll()), cacheable);
}

std::optional<QJsonObject> jsonBody(const QHttpServerRequest &request)
{
    if (request.body().size() > kMaxJsonBody)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(request.body(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::nullopt;
    return doc.object();
}

QString sessionToken(const QHttpServerRequest &request)
{
    const QByteArray cookies = request.headers().value(QHttpHeaders::WellKnownHeader::Cookie).toByteArray();
    const QByteArray prefix = QByteArray(kSessionCookie) + '=';
    for (const QByteArray &part : cookies.split(';')) {
        const QByteArray item = part.trimmed();
        if (item.startsWith(prefix))
            return QString::fromLatin1(item.mid(prefix.size()));
    }
    return {};
}

QByteArray sessionCookie(const QString &token, qint64 maxAgeSeconds)
{
    return QByteArray(kSessionCookie) + '=' + token.toLatin1() + "; Path=/; HttpOnly; SameSite=Strict; Max-Age="
        + QByteArray::number(maxAgeSeconds);
}

QHttpServerResponse withCookie(QHttpServerResponse response, const QByteArray &cookie)
{
    QHttpHeaders headers = response.headers();
    headers.append(QHttpHeaders::WellKnownHeader::SetCookie, cookie);
    response.setHeaders(std::move(headers));
    return response;
}

// Only the documented shape is accepted, so the display never gets a config it cannot read
QString validateConfig(const QJsonObject &config)
{
    const auto expect = [&](QStringView key, QJsonValue::Type type, const char *name) -> QString {
        const QJsonValue value = config.value(key);
        if (!value.isUndefined() && value.type() != type)
            return QStringLiteral("\"%1\" muss %2 sein").arg(key, QLatin1String(name));
        return {};
    };
    for (const QString &error : {
             expect(u"display", QJsonValue::Object, "ein Objekt"),
             expect(u"media", QJsonValue::Object, "ein Objekt"),
             expect(u"library", QJsonValue::Object, "ein Objekt"),
             expect(u"admin", QJsonValue::Object, "ein Objekt"),
             expect(u"layouts", QJsonValue::Array, "eine Liste"),
             expect(u"quotes", QJsonValue::Array, "eine Liste"),
             expect(u"activeLayout", QJsonValue::String, "ein Text"),
             expect(u"locale", QJsonValue::String, "ein Text"),
         }) {
        if (!error.isEmpty())
            return error;
    }
    for (const QJsonValue &layout : config.value(u"layouts").toArray()) {
        if (!layout.isObject())
            return QStringLiteral("Jedes Layout muss ein Objekt sein");
        const QJsonValue regions = layout.toObject().value(u"regions");
        if (!regions.isUndefined() && !regions.isArray())
            return QStringLiteral("\"regions\" muss eine Liste sein");
        for (const QJsonValue &region : regions.toArray()) {
            if (!region.isObject() || !region.toObject().value(u"type").isString())
                return QStringLiteral("Jede Region braucht einen \"type\"");
        }
    }
    return {};
}

} // namespace

ApiServer::ApiServer(const QString &configPath, DisplayLink *display, QObject *parent)
    : QObject(parent)
    , m_configPath(configPath)
    , m_display(display)
{
    m_uptime.start();
    addRoutes();
}

ApiServer::~ApiServer() = default;

bool ApiServer::listen(const QHostAddress &address, quint16 port, QString *error)
{
    m_tcp = new QTcpServer(this);
    if (!m_tcp->listen(address, port) || !m_http.bind(m_tcp)) {
        if (error)
            *error = m_tcp->errorString();
        delete m_tcp;
        m_tcp = nullptr;
        return false;
    }
    return true;
}

quint16 ApiServer::serverPort() const
{
    return m_tcp ? m_tcp->serverPort() : 0;
}

berry::Config ApiServer::config() const
{
    // Small file; reading it per request means edits apply without a restart
    return berry::Config::load(m_configPath);
}

Credentials ApiServer::credentials() const
{
    return loadCredentials(credentialsPath(config().library().stateDir));
}

std::optional<QHttpServerResponse> ApiServer::checkAccess(const QHttpServerRequest &request, bool changes)
{
    if (changes) {
        const QHttpHeaders headers = request.headers();
        if (!headers.contains(kCsrfHeader))
            return fail(Status::Forbidden, QStringLiteral("Header %1 fehlt").arg(QLatin1String(kCsrfHeader)));
        // Browsers send Origin on cross-site requests; it has to be us
        const QByteArray origin = headers.value(QHttpHeaders::WellKnownHeader::Origin).toByteArray();
        const QByteArray host = headers.value(QHttpHeaders::WellKnownHeader::Host).toByteArray();
        if (!origin.isEmpty() && QUrl(QString::fromLatin1(origin)).authority().toLatin1() != host)
            return fail(Status::Forbidden, QStringLiteral("Fremde Herkunft"));
    }

    const QString token = sessionToken(request);
    if (token.isEmpty() || !m_sessions.validate(token, credentials().stamp()))
        return fail(Status::Unauthorized, QStringLiteral("Bitte anmelden"));
    return std::nullopt;
}

void ApiServer::addRoutes()
{
    // Built-in web UI
    m_http.route(QStringLiteral("/"), Method::Get, [] { return serveFile(QStringLiteral(":/www/index.html"), false); });
    m_http.route(QStringLiteral("/app.js"), Method::Get, [] { return serveFile(QStringLiteral(":/www/app.js"), false); });
    m_http.route(QStringLiteral("/app.css"), Method::Get, [] { return serveFile(QStringLiteral(":/www/app.css"), false); });

    m_http.route(QStringLiteral("/api/auth/session"), Method::Get,
                 [this](const QHttpServerRequest &request) { return session(request); });
    m_http.route(QStringLiteral("/api/auth/login"), Method::Post,
                 [this](const QHttpServerRequest &request) { return login(request); });
    m_http.route(QStringLiteral("/api/auth/logout"), Method::Post,
                 [this](const QHttpServerRequest &request) { return logout(request); });

    m_http.route(QStringLiteral("/api/status"), Method::Get, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return status();
    });

    m_http.route(QStringLiteral("/api/media"), Method::Get, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return listMedia(request);
    });
    m_http.route(QStringLiteral("/api/media"), Method::Post, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, true))
            return std::move(*denied);
        return upload(request);
    });
    m_http.route(QStringLiteral("/api/media/<arg>"), Method::Get, [this](qint64 id, const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return getMedia(id);
    });
    m_http.route(QStringLiteral("/api/media/<arg>"), Method::Patch, [this](qint64 id, const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, true))
            return std::move(*denied);
        return updateMedia(id, request);
    });
    m_http.route(QStringLiteral("/api/media/<arg>"), Method::Delete, [this](qint64 id, const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, true))
            return std::move(*denied);
        return deleteMedia(id);
    });
    m_http.route(QStringLiteral("/api/media/<arg>/thumb"), Method::Get, [this](qint64 id, const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return mediaFile(id, QStringLiteral("thumb"));
    });
    m_http.route(QStringLiteral("/api/media/<arg>/image"), Method::Get, [this](qint64 id, const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return mediaFile(id, request.query().queryItemValue(QStringLiteral("kind")));
    });

    m_http.route(QStringLiteral("/api/albums"), Method::Get, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return listAlbums();
    });
    m_http.route(QStringLiteral("/api/albums"), Method::Post, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, true))
            return std::move(*denied);
        return createAlbum(request);
    });

    m_http.route(QStringLiteral("/api/config"), Method::Get, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return getConfig();
    });
    m_http.route(QStringLiteral("/api/config"), Method::Put, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, true))
            return std::move(*denied);
        return putConfig(request);
    });

    for (const char *command : {"next", "pause", "resume"}) {
        const QString name = QLatin1String(command);
        m_http.route(QStringLiteral("/api/display/") + name, Method::Post, [this, name](const QHttpServerRequest &request) {
            if (auto denied = checkAccess(request, true))
                return std::move(*denied);
            return displayCommand(name);
        });
    }
    m_http.route(QStringLiteral("/api/display/screenshot"), Method::Get, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, false))
            return std::move(*denied);
        return screenshot(request);
    });

    m_http.route(QStringLiteral("/api/library/sync"), Method::Post, [this](const QHttpServerRequest &request) {
        if (auto denied = checkAccess(request, true))
            return std::move(*denied);
        emit syncRequested();
        return empty(Status::Accepted);
    });
}

QHttpServerResponse ApiServer::session(const QHttpServerRequest &request)
{
    const Credentials stored = credentials();
    QJsonObject result{{QStringLiteral("authenticated"), false}, {QStringLiteral("passwordSet"), stored.isValid()}};
    const QString token = sessionToken(request);
    if (!token.isEmpty()) {
        if (const auto username = m_sessions.validate(token, stored.stamp())) {
            result.insert(QStringLiteral("authenticated"), true);
            result.insert(QStringLiteral("username"), *username);
        }
    }
    return json(result);
}

QHttpServerResponse ApiServer::login(const QHttpServerRequest &request)
{
    const QHostAddress address = request.remoteAddress();
    qint64 retryAfterMs = 0;
    if (!m_throttle.allowed(address, &retryAfterMs)) {
        const qint64 seconds = (retryAfterMs + 999) / 1000;
        QHttpServerResponse response = json(QJsonObject{
            {QStringLiteral("error"), QStringLiteral("Zu viele Fehlversuche, bitte in %1 s erneut versuchen").arg(seconds)},
            {QStringLiteral("retryAfter"), seconds},
        }, Status::TooManyRequests);
        QHttpHeaders headers = response.headers();
        headers.append(QHttpHeaders::WellKnownHeader::RetryAfter, QByteArray::number(seconds));
        response.setHeaders(std::move(headers));
        return response;
    }

    const Credentials stored = credentials();
    if (!stored.isValid()) {
        return fail(Status::ServiceUnavailable,
                    QStringLiteral("Noch kein Passwort gesetzt. Auf dem Pi: sudo -u berry-kiosk berry-kiosk-admin "
                                   "--config /var/lib/berry-kiosk/config.json --set-password"));
    }

    const auto body = jsonBody(request);
    if (!body)
        return fail(Status::BadRequest, QStringLiteral("JSON mit username und password erwartet"));
    const QString username = body->value(u"username").toString();
    const QString password = body->value(u"password").toString();
    if (!stored.verify(username, password)) {
        m_throttle.recordFailure(address);
        qWarning().noquote() << "Anmeldung fehlgeschlagen von" << address.toString();
        return fail(Status::Unauthorized, QStringLiteral("Benutzername oder Passwort falsch"));
    }

    m_throttle.recordSuccess(address);
    const QString token = m_sessions.create(stored.username, stored.stamp());
    qInfo().noquote() << "Angemeldet:" << stored.username << "von" << address.toString();
    return withCookie(json(QJsonObject{{QStringLiteral("username"), stored.username}}),
                      sessionCookie(token, m_sessions.lifetimeMs() / 1000));
}

QHttpServerResponse ApiServer::logout(const QHttpServerRequest &request)
{
    if (!request.headers().contains(kCsrfHeader))
        return fail(Status::Forbidden, QStringLiteral("Header %1 fehlt").arg(QLatin1String(kCsrfHeader)));
    m_sessions.revoke(sessionToken(request));
    return withCookie(empty(), sessionCookie(QString(), 0));
}

QHttpServerResponse ApiServer::status()
{
    const berry::Config cfg = config();
    const berry::LibrarySettings library = cfg.library();

    QJsonObject display = berry::readJsonFile(berry::displayStatePath(library.stateDir));
    display.insert(QStringLiteral("connected"), m_display->isConnected());
    if (m_display->isConnected()) {
        const QCborMap reply = m_display->request(QStringLiteral("status"), {}, 1500);
        if (!reply.isEmpty()) {
            display.insert(QStringLiteral("paused"), reply.value(QStringLiteral("paused")).toBool());
            display.insert(QStringLiteral("layout"), reply.value(QStringLiteral("layout")).toString());
        }
        // Map the originals the display reports to library ids, for thumbnails
        QJsonArray showing;
        const QHash<QString, QString> shown = m_display->shown();
        const QDir media(cfg.mediaRoot());
        for (auto it = shown.cbegin(); it != shown.cend(); ++it) {
            const QString relative = media.relativeFilePath(it.value());
            QJsonObject item{{QStringLiteral("region"), it.key()}, {QStringLiteral("path"), relative}};
            if (const qint64 id = berry::mediaIdByPath(library.database, relative))
                item.insert(QStringLiteral("id"), id);
            showing.append(item);
        }
        display.insert(QStringLiteral("showing"), showing);
    }

    QJsonObject libraryState = berry::readJsonFile(berry::libraryStatePath(library.stateDir));
    libraryState.remove(QStringLiteral("generation"));

    const QStorageInfo storage(cfg.mediaRoot());
    return json(QJsonObject{
        {QStringLiteral("version"), QStringLiteral(BERRY_KIOSK_VERSION)},
        {QStringLiteral("uptimeSeconds"), m_uptime.elapsed() / 1000},
        {QStringLiteral("display"), display},
        {QStringLiteral("library"), libraryState},
        {QStringLiteral("storage"), QJsonObject{
             {QStringLiteral("free"), storage.bytesAvailable()},
             {QStringLiteral("total"), storage.bytesTotal()},
         }},
    });
}

QHttpServerResponse ApiServer::listMedia(const QHttpServerRequest &request)
{
    const QUrlQuery query = request.query();
    berry::MediaQuery mediaQuery;
    mediaQuery.album = query.queryItemValue(QStringLiteral("album"), QUrl::FullyDecoded);
    mediaQuery.offset = query.queryItemValue(QStringLiteral("offset")).toInt();
    mediaQuery.limit = query.hasQueryItem(QStringLiteral("limit")) ? query.queryItemValue(QStringLiteral("limit")).toInt() : 100;

    QString error;
    const QJsonObject result = berry::queryMedia(config().library().database, mediaQuery, &error);
    if (result.isEmpty()) {
        // No library yet is not an error, just an empty one
        return json(QJsonObject{{QStringLiteral("total"), 0}, {QStringLiteral("items"), QJsonArray()}});
    }
    return json(result);
}

QHttpServerResponse ApiServer::getMedia(qint64 id)
{
    const QJsonObject media = berry::mediaById(config().library().database, id);
    if (media.isEmpty())
        return fail(Status::NotFound, QStringLiteral("Bild %1 unbekannt").arg(id));
    return json(media);
}

QHttpServerResponse ApiServer::updateMedia(qint64 id, const QHttpServerRequest &request)
{
    const auto body = jsonBody(request);
    if (!body)
        return fail(Status::BadRequest, QStringLiteral("JSON-Objekt erwartet"));

    // Only these keys may be changed; everything else in the sidecar stays as it is
    static const QStringList textKeys = {
        QStringLiteral("title"), QStringLiteral("description"), QStringLiteral("location"),
        QStringLiteral("quote"), QStringLiteral("quoteAuthor"),
    };
    QJsonObject changes;
    for (auto it = body->constBegin(); it != body->constEnd(); ++it) {
        if (textKeys.contains(it.key())) {
            if (!it.value().isString() && !it.value().isNull())
                return fail(Status::BadRequest, QStringLiteral("\"%1\" muss Text oder null sein").arg(it.key()));
            if (it.value().toString().size() > kMaxTextField)
                return fail(Status::BadRequest, QStringLiteral("\"%1\" ist zu lang").arg(it.key()));
            changes.insert(it.key(), it.value());
        } else if (it.key() == u"enabled") {
            if (!it.value().isBool())
                return fail(Status::BadRequest, QStringLiteral("\"enabled\" muss true oder false sein"));
            // Shown is the default; keep the sidecar small
            changes.insert(it.key(), it.value().toBool() ? QJsonValue(QJsonValue::Null) : QJsonValue(false));
        } else {
            return fail(Status::BadRequest, QStringLiteral("\"%1\" lässt sich nicht ändern").arg(it.key()));
        }
    }

    const berry::Config cfg = config();
    const QJsonObject media = berry::mediaById(cfg.library().database, id);
    if (media.isEmpty())
        return fail(Status::NotFound, QStringLiteral("Bild %1 unbekannt").arg(id));

    QString error;
    const QString image = QDir(cfg.mediaRoot()).filePath(media.value(u"path").toString());
    if (!berry::updateSidecar(image, changes, &error))
        return fail(Status::InternalServerError, error);
    emit syncRequested();
    return json(QJsonObject{{QStringLiteral("ok"), true}}, Status::Accepted);
}

QHttpServerResponse ApiServer::deleteMedia(qint64 id)
{
    const berry::Config cfg = config();
    const QJsonObject media = berry::mediaById(cfg.library().database, id);
    if (media.isEmpty())
        return fail(Status::NotFound, QStringLiteral("Bild %1 unbekannt").arg(id));

    QString error;
    const QString image = QDir(cfg.mediaRoot()).filePath(media.value(u"path").toString());
    if (!berry::moveToTrash(cfg.mediaRoot(), cfg.library().trashDir, image, &error))
        return fail(Status::InternalServerError, error);
    qInfo().noquote() << "In den Papierkorb:" << media.value(u"path").toString();
    emit syncRequested();
    return empty();
}

QHttpServerResponse ApiServer::mediaFile(qint64 id, const QString &kind)
{
    const berry::Config cfg = config();
    const QJsonObject media = berry::mediaById(cfg.library().database, id);
    if (media.isEmpty())
        return fail(Status::NotFound, QStringLiteral("Bild %1 unbekannt").arg(id));

    const QString cacheRoot = cfg.library().cacheRoot;
    const QString original = QDir(cfg.mediaRoot()).filePath(media.value(u"path").toString());
    if (kind == u"original")
        return serveFile(original, true);

    // A specific rendition, or the best one there is
    const QStringList candidates = kind.isEmpty()
        ? QStringList{QStringLiteral("2160p"), QStringLiteral("1080p")}
        : QStringList{kind};
    for (const QString &candidate : candidates) {
        const QString file = berry::renditionFilePath(cacheRoot, id, candidate);
        if (QFileInfo::exists(file))
            return serveFile(file, true);
    }
    if (kind.isEmpty())
        return serveFile(original, true);
    return fail(Status::NotFound, QStringLiteral("Variante %1 noch nicht erstellt").arg(kind));
}

QHttpServerResponse ApiServer::upload(const QHttpServerRequest &request)
{
    const berry::Config cfg = config();
    const QByteArray &body = request.body();
    if (body.isEmpty())
        return fail(Status::BadRequest, QStringLiteral("Leere Datei"));
    if (body.size() > cfg.admin().maxUploadBytes) {
        return fail(Status::PayloadTooLarge,
                    QStringLiteral("Datei größer als %1 MB").arg(cfg.admin().maxUploadBytes / (1024 * 1024)));
    }

    // Trust the content, not the name or the Content-Type
    QBuffer buffer;
    buffer.setData(body);
    buffer.open(QIODevice::ReadOnly);
    const QByteArray format = QImageReader::imageFormat(&buffer).toLower();
    buffer.seek(0);
    const QSize size = QImageReader(&buffer, format).size();
    if (!QByteArrayList{"jpeg", "png", "webp"}.contains(format) || size.isEmpty())
        return fail(Status::UnsupportedMediaType, QStringLiteral("Nur JPEG, PNG und WebP"));

    const QUrlQuery query = request.query();
    const QString album = query.queryItemValue(QStringLiteral("album"), QUrl::FullyDecoded);
    const QString directory = berry::albumDirectory(cfg.mediaRoot(), album);
    if (directory.isEmpty())
        return fail(Status::BadRequest, QStringLiteral("Ungültiges Album"));
    const QString name = berry::sanitizeFileName(query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded), format);
    if (!QDir().mkpath(directory))
        return fail(Status::InternalServerError, QStringLiteral("Album nicht anlegbar"));

    const QString target = berry::uniqueFilePath(directory, name);
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size() || !file.commit())
        return fail(Status::InternalServerError, QStringLiteral("Speichern fehlgeschlagen: %1").arg(file.errorString()));

    const QDir root(cfg.mediaRoot());
    const QString relative = root.relativeFilePath(target);
    qInfo().noquote() << "Hochgeladen:" << relative << QStringLiteral("(%1 KB)").arg(body.size() / 1024);
    emit syncRequested();
    return json(QJsonObject{
        {QStringLiteral("path"), relative},
        {QStringLiteral("album"), directory == QDir::cleanPath(cfg.mediaRoot()) ? QString(u"") : root.relativeFilePath(directory)},
    }, Status::Created);
}

QHttpServerResponse ApiServer::listAlbums()
{
    const berry::Config cfg = config();
    QMap<QString, QJsonObject> albums;
    for (const QJsonValue &value : berry::albumCounts(cfg.library().database)) {
        const QJsonObject album = value.toObject();
        albums.insert(album.value(u"name").toString(), album);
    }

    // Folders without pictures (yet) are albums too, e.g. right after creating one
    const QDir root(cfg.mediaRoot());
    QDirIterator it(cfg.mediaRoot(), QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString relative = root.relativeFilePath(it.next());
        if (berry::albumDirectory(cfg.mediaRoot(), relative).isEmpty())
            continue; // hidden folders
        if (!albums.contains(relative)) {
            albums.insert(relative, QJsonObject{{QStringLiteral("name"), relative},
                                                {QStringLiteral("count"), 0},
                                                {QStringLiteral("enabled"), 0}});
        }
    }
    if (!albums.contains(QString(u"")))
        albums.insert(QString(u""), QJsonObject{{QStringLiteral("name"), QString(u"")}, {QStringLiteral("count"), 0}, {QStringLiteral("enabled"), 0}});

    QJsonArray result;
    QStringList names = albums.keys();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    for (const QString &name : std::as_const(names))
        result.append(albums.value(name));
    return json(result);
}

QHttpServerResponse ApiServer::createAlbum(const QHttpServerRequest &request)
{
    const auto body = jsonBody(request);
    const QString name = body ? body->value(u"name").toString().trimmed() : QString();
    const berry::Config cfg = config();
    const QString directory = berry::albumDirectory(cfg.mediaRoot(), name);
    if (name.isEmpty() || directory.isEmpty() || directory == QDir::cleanPath(cfg.mediaRoot()))
        return fail(Status::BadRequest, QStringLiteral("Ungültiger Albumname"));
    if (!QDir().mkpath(directory))
        return fail(Status::InternalServerError, QStringLiteral("Album nicht anlegbar"));
    return json(QJsonObject{{QStringLiteral("name"), QDir(cfg.mediaRoot()).relativeFilePath(directory)}}, Status::Created);
}

QHttpServerResponse ApiServer::getConfig()
{
    return json(berry::readJsonFile(m_configPath));
}

QHttpServerResponse ApiServer::putConfig(const QHttpServerRequest &request)
{
    const auto body = jsonBody(request);
    if (!body)
        return fail(Status::BadRequest, QStringLiteral("JSON-Objekt erwartet"));
    if (const QString error = validateConfig(*body); !error.isEmpty())
        return fail(Status::BadRequest, error);

    // Keep the previous version, a broken layout is one copy away from undone
    const QString backup = m_configPath + QStringLiteral(".bak");
    if (QFileInfo::exists(m_configPath)) {
        QFile::remove(backup);
        QFile::copy(m_configPath, backup);
    }
    QString error;
    if (!berry::writeJsonFile(m_configPath, *body, &error))
        return fail(Status::InternalServerError, error);
    qInfo() << "Konfiguration über die Web-API geändert";
    return empty(); // display and library worker watch the file
}

QHttpServerResponse ApiServer::displayCommand(const QString &command)
{
    QString error;
    if (m_display->request(command, {}, 2000, &error).isEmpty())
        return fail(Status::ServiceUnavailable, error);
    return empty();
}

QHttpServerResponse ApiServer::screenshot(const QHttpServerRequest &request)
{
    const int maxWidth = request.query().hasQueryItem(QStringLiteral("maxWidth"))
        ? request.query().queryItemValue(QStringLiteral("maxWidth")).toInt()
        : 1280;
    QString error;
    const QCborMap reply = m_display->request(QStringLiteral("screenshot"),
                                              {{QStringLiteral("maxWidth"), maxWidth}}, 5000, &error);
    if (reply.isEmpty())
        return fail(Status::ServiceUnavailable, error);
    return finish(QHttpServerResponse("image/jpeg", reply.value(QStringLiteral("image")).toByteArray()));
}
