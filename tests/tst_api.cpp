#include "apiserver.h"
#include "auth.h"
#include "displaylink.h"
#include "imagemetadata.h"
#include "library.h"
#include "state.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

#include <exiv2/exiv2.hpp>

using namespace berry;

namespace {

struct Response
{
    int status = 0;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers; // lower-case names

    QJsonObject object() const { return QJsonDocument::fromJson(body).object(); }
    QJsonArray array() const { return QJsonDocument::fromJson(body).array(); }
    QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

QByteArray jpegBytes(QSize size = QSize(64, 48))
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG");
    return bytes;
}

QByteArray pngBytes()
{
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::darkBlue);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QByteArray jsonBytes(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// One kiosk data directory with a running API server on a random port
struct Env
{
    QTemporaryDir dir;
    QString configPath;
    DisplayLink link;
    std::unique_ptr<ApiServer> server;
    QNetworkAccessManager nam;

    QString path(const QString &relative) const { return dir.filePath(relative); }
    QString media() const { return path(QStringLiteral("media")); }

    Response request(const QByteArray &method, const QString &target, const QByteArray &body = {},
                     const QList<QPair<QByteArray, QByteArray>> &headers = {}, bool csrf = true)
    {
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server->serverPort()).arg(target)));
        if (csrf && method != "GET")
            request.setRawHeader("X-Berry-Kiosk", "1");
        for (const auto &[name, value] : headers)
            request.setRawHeader(name, value);
        if (method != "GET" && !request.hasRawHeader("Content-Type"))
            request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));

        QNetworkReply *reply = nam.sendCustomRequest(request, method, body);
        QSignalSpy finished(reply, &QNetworkReply::finished);
        if (!reply->isFinished())
            finished.wait(10000);

        Response response;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        for (const auto &[name, value] : reply->rawHeaderPairs())
            response.headers.insert(name.toLower(), value);
        reply->deleteLater();
        return response;
    }

    Response get(const QString &target) { return request("GET", target); }

    Response post(const QString &target, const QJsonObject &object)
    {
        return request("POST", target, jsonBytes(object), {{"Content-Type", "application/json"}});
    }

    bool login(const QString &password = QStringLiteral("geheim123"))
    {
        return post(QStringLiteral("/api/auth/login"),
                    {{QStringLiteral("username"), QStringLiteral("admin")}, {QStringLiteral("password"), password}})
                   .status
            == 200;
    }

    // What the admin worker would do after syncRequested()
    void sync()
    {
        const LibrarySettings settings = Config::load(configPath).library();
        Library library(settings.database, Config::load(configPath).mediaRoot(), settings.cacheRoot);
        QVERIFY(library.open());
        library.sync();
    }
};

std::unique_ptr<Env> makeEnv(const QJsonObject &extraConfig = {}, bool withPassword = true)
{
    auto env = std::make_unique<Env>();
    if (!env->dir.isValid())
        return nullptr;
    QJsonObject config{{QStringLiteral("media"), QJsonObject{{QStringLiteral("path"), QStringLiteral("media")}}}};
    for (auto it = extraConfig.constBegin(); it != extraConfig.constEnd(); ++it)
        config.insert(it.key(), it.value());
    env->configPath = env->path(QStringLiteral("config.json"));
    if (!writeJsonFile(env->configPath, config) || !QDir().mkpath(env->media()))
        return nullptr;
    if (withPassword) {
        const QString stateDir = env->path(QStringLiteral("state"));
        if (!saveCredentials(credentialsPath(stateDir), Credentials::create(QStringLiteral("admin"), QStringLiteral("geheim123"), 1000)))
            return nullptr;
    }
    env->server = std::make_unique<ApiServer>(env->configPath, &env->link);
    if (!env->server->listen(QHostAddress::LocalHost, 0))
        return nullptr;
    return env;
}

QStringList filesBelow(const QString &directory)
{
    QStringList files;
    QDirIterator it(directory, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files.append(QDir(directory).relativeFilePath(it.next()));
    files.sort();
    return files;
}

// Sets the EXIF capture date of a picture in the media folder
void setTaken(const QString &path, const char *date)
{
    auto image = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
    image->readMetadata();
    image->exifData()["Exif.Photo.DateTimeOriginal"] = std::string(date);
    image->writeMetadata();
}

QStringList pathsOf(const QJsonArray &items)
{
    QStringList paths;
    for (const QJsonValue &item : items)
        paths.append(item.toObject().value(u"path").toString());
    return paths;
}

qint64 idOf(const QJsonArray &items, const QString &path)
{
    for (const QJsonValue &item : items) {
        if (item.toObject().value(u"path").toString() == path)
            return item.toObject().value(u"id").toInteger();
    }
    return 0;
}

// Plain HTTP over a socket, so the Host header is exactly what we say. Waits
// with the event loop running: the server lives in this thread.
int rawStatus(quint16 port, const QByteArray &method, const QByteArray &target, const QByteArray &host,
              const QList<QByteArray> &headers = {}, const QByteArray &body = {})
{
    QTcpSocket socket;
    QByteArray reply;
    QObject::connect(&socket, &QTcpSocket::readyRead, [&] { reply += socket.readAll(); });
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!QTest::qWaitFor([&] { return socket.state() == QAbstractSocket::ConnectedState; }, 5000))
        return -1;

    QByteArray request = method + ' ' + target + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n";
    for (const QByteArray &header : headers)
        request += header + "\r\n";
    if (method != "GET")
        request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    request += "\r\n" + body;
    socket.write(request);

    if (!QTest::qWaitFor([&] { return reply.contains("\r\n"); }, 5000))
        return -1;
    // "HTTP/1.1 403 Forbidden"
    return reply.left(reply.indexOf("\r\n")).split(' ').value(1).toInt();
}

QByteArray sessionCookie(QNetworkAccessManager &nam, quint16 port)
{
    for (const QNetworkCookie &cookie : nam.cookieJar()->cookiesForUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(port)))) {
        if (cookie.name() == "berry_session")
            return "Cookie: berry_session=" + cookie.value();
    }
    return {};
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffu;
    for (const char byte : data) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

QByteArray pngChunk(const QByteArray &type, const QByteArray &data)
{
    QByteArray chunk;
    const auto be32 = [&chunk](quint32 value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            chunk.append(char((value >> shift) & 0xff));
    };
    be32(quint32(data.size()));
    chunk += type + data;
    be32(crc32(type + data));
    return chunk;
}

// A few dozen bytes that claim width × height pixels: a decompression bomb's header
QByteArray pngHeaderOnly(quint32 width, quint32 height)
{
    QByteArray header;
    for (const quint32 value : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8)
            header.append(char((value >> shift) & 0xff));
    }
    header += QByteArray::fromHex("0802000000"); // 8 bit RGB, no interlace
    // libpng reads up to the first IDAT before it reports the size; a tiny
    // zlib stream (qCompress minus its length prefix) is enough for that
    const QByteArray idat = qCompress(QByteArray(16, '\0')).mid(4);
    return QByteArray::fromHex("89504e470d0a1a0a") + pngChunk("IHDR", header) + pngChunk("IDAT", idat)
        + pngChunk("IEND", {});
}

} // namespace

class TestApi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void staticFiles();
    void authFlow();
    void noPasswordSet();
    void passwordChangeEndsSessions();
    void loginThrottle();
    void csrf();
    void upload();
    void uploadLimits();
    void mediaAndLibrary();
    void albums();
    void config();
    void displayOffline();
    void previewLayout();
    void screenSwitch();
    void publicProfileLogin();
    void mediaFilters();
    void albumCovers();
    void batch();
    void batchErrors();
    void renameAlbum();
    void deleteAlbum();
    void changePassword();
    void hostHeader();
    void loginNeedsCsrfHeader();
    void changePasswordThrottle();
    void imageKindAllowlist();
    void configPathsLocked();
    void uploadPixelLimit();
    void credentialsFixedLocation();
};

void TestApi::initTestCase()
{
    initializeMetadataReading();
    if (!QFile::exists(QStringLiteral(":/www/index.html")))
        Q_INIT_RESOURCE(www);
    QVERIFY(QFile::exists(QStringLiteral(":/www/index.html")));
}

void TestApi::staticFiles()
{
    const auto env = makeEnv();
    QVERIFY(env);

    const Response index = env->get(QStringLiteral("/"));
    QCOMPARE(index.status, 200);
    QVERIFY(index.header("Content-Type").startsWith("text/html"));
    QVERIFY(index.header("Content-Security-Policy").contains("default-src 'self'"));
    QCOMPARE(index.header("X-Frame-Options"), QByteArray("DENY"));
    QCOMPARE(index.header("X-Content-Type-Options"), QByteArray("nosniff"));

    const Response script = env->get(QStringLiteral("/js/main.js"));
    QCOMPARE(script.status, 200);
    QVERIFY(script.header("Content-Type").startsWith("text/javascript"));
    QVERIFY(env->get(QStringLiteral("/css/base.css")).header("Content-Type").startsWith("text/css"));
    const Response icon = env->get(QStringLiteral("/assets/icon.svg"));
    QCOMPARE(icon.status, 200);
    QCOMPARE(icon.header("Content-Type"), QByteArray("image/svg+xml"));

    // Only plain file names inside the bundled folders
    QCOMPARE(env->get(QStringLiteral("/js/..%2Fsecret")).status, 404);
    QCOMPARE(env->get(QStringLiteral("/js/.hidden.js")).status, 404);
    QCOMPARE(env->get(QStringLiteral("/js/gibt-es-nicht.js")).status, 404);
    QCOMPARE(env->get(QStringLiteral("/app.js")).status, 404); // the milestone 4 console is gone
}

void TestApi::authFlow()
{
    const auto env = makeEnv();
    QVERIFY(env);

    QJsonObject session = env->get(QStringLiteral("/api/auth/session")).object();
    QCOMPARE(session.value(u"authenticated").toBool(), false);
    QCOMPARE(session.value(u"passwordSet").toBool(), true);
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 401);

    QCOMPARE(env->post(QStringLiteral("/api/auth/login"),
                       {{QStringLiteral("username"), QStringLiteral("admin")}, {QStringLiteral("password"), QStringLiteral("falsch")}})
                 .status,
             401);
    QCOMPARE(env->post(QStringLiteral("/api/auth/login"),
                       {{QStringLiteral("username"), QStringLiteral("root")}, {QStringLiteral("password"), QStringLiteral("geheim123")}})
                 .status,
             401);
    QCOMPARE(env->request("POST", QStringLiteral("/api/auth/login"), "kein json").status, 400);

    const Response login = env->post(QStringLiteral("/api/auth/login"),
                                     {{QStringLiteral("username"), QStringLiteral("admin")},
                                      {QStringLiteral("password"), QStringLiteral("geheim123")}});
    QCOMPARE(login.status, 200);
    QCOMPARE(login.object().value(u"username").toString(), QStringLiteral("admin"));
    const QByteArray cookie = login.header("Set-Cookie");
    QVERIFY2(cookie.contains("berry_session="), cookie.constData());
    QVERIFY(cookie.contains("HttpOnly"));
    QVERIFY(cookie.contains("SameSite=Strict"));

    session = env->get(QStringLiteral("/api/auth/session")).object();
    QCOMPARE(session.value(u"authenticated").toBool(), true);
    QCOMPARE(session.value(u"username").toString(), QStringLiteral("admin"));
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 200);

    QCOMPARE(env->request("POST", QStringLiteral("/api/auth/logout"), {}, {}, false).status, 403);
    QCOMPARE(env->request("POST", QStringLiteral("/api/auth/logout")).status, 204);
    session = env->get(QStringLiteral("/api/auth/session")).object();
    QCOMPARE(session.value(u"authenticated").toBool(), false);
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 401);
}

void TestApi::noPasswordSet()
{
    const auto env = makeEnv({}, false);
    QVERIFY(env);
    QCOMPARE(env->get(QStringLiteral("/api/auth/session")).object().value(u"passwordSet").toBool(), false);
    const Response login = env->post(QStringLiteral("/api/auth/login"),
                                     {{QStringLiteral("username"), QStringLiteral("admin")},
                                      {QStringLiteral("password"), QStringLiteral("geheim123")}});
    QCOMPARE(login.status, 503);
    QVERIFY(login.object().value(u"error").toString().contains(QStringLiteral("--set-password")));
}

void TestApi::passwordChangeEndsSessions()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 200);

    QVERIFY(saveCredentials(credentialsPath(env->path(QStringLiteral("state"))),
                            Credentials::create(QStringLiteral("admin"), QStringLiteral("neuesPasswort"), 1000)));
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 401);
    QVERIFY(!env->login());
    QVERIFY(env->login(QStringLiteral("neuesPasswort")));
}

void TestApi::loginThrottle()
{
    const auto env = makeEnv();
    QVERIFY(env);
    for (int i = 0; i < 5; ++i)
        QVERIFY(!env->login(QStringLiteral("falsch")));

    const Response blocked = env->post(QStringLiteral("/api/auth/login"),
                                       {{QStringLiteral("username"), QStringLiteral("admin")},
                                        {QStringLiteral("password"), QStringLiteral("geheim123")}});
    QCOMPARE(blocked.status, 429);
    QVERIFY(blocked.object().value(u"retryAfter").toInt() > 0);
    QVERIFY(!blocked.header("Retry-After").isEmpty());
}

void TestApi::csrf()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QSignalSpy sync(env->server.get(), &ApiServer::syncRequested);

    QCOMPARE(env->request("POST", QStringLiteral("/api/library/sync"), {}, {}, false).status, 403);
    QCOMPARE(sync.count(), 0);
    QCOMPARE(env->request("POST", QStringLiteral("/api/library/sync")).status, 202);
    QCOMPARE(sync.count(), 1);
    QCOMPARE(env->request("POST", QStringLiteral("/api/library/sync"), {}, {{"Origin", "http://evil.example"}}).status, 403);
    QCOMPARE(sync.count(), 1);

    // Our own origin is fine
    const QByteArray own = QStringLiteral("http://127.0.0.1:%1").arg(env->server->serverPort()).toLatin1();
    QCOMPARE(env->request("POST", QStringLiteral("/api/library/sync"), {}, {{"Origin", own}}).status, 202);
}

void TestApi::upload()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QSignalSpy sync(env->server.get(), &ApiServer::syncRequested);

    // Not logged in → no upload
    {
        QNetworkAccessManager anonymous;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/api/media?name=x.jpg").arg(env->server->serverPort())));
        request.setRawHeader("X-Berry-Kiosk", "1");
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("image/jpeg"));
        QNetworkReply *reply = anonymous.post(request, jpegBytes());
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
        reply->deleteLater();
    }

    Response first = env->request("POST", QStringLiteral("/api/media?album=alpen&name=Elch.jpg"), jpegBytes(),
                                  {{"Content-Type", "image/jpeg"}});
    QCOMPARE(first.status, 201);
    QCOMPARE(first.object().value(u"path").toString(), QStringLiteral("alpen/Elch.jpg"));
    QCOMPARE(first.object().value(u"album").toString(), QStringLiteral("alpen"));
    QVERIFY(QFile::exists(env->media() + QStringLiteral("/alpen/Elch.jpg")));
    QCOMPARE(sync.count(), 1);

    const Response second = env->request("POST", QStringLiteral("/api/media?album=alpen&name=Elch.jpg"), jpegBytes(),
                                         {{"Content-Type", "image/jpeg"}});
    QCOMPARE(second.status, 201);
    QCOMPARE(second.object().value(u"path").toString(), QStringLiteral("alpen/Elch (2).jpg"));

    // Root album, umlauts and spaces in the name survive URL encoding
    const Response root = env->request("POST", QStringLiteral("/api/media?name=%C3%84sche%20am%20Bach.jpg"), jpegBytes());
    QCOMPARE(root.status, 201);
    QCOMPARE(root.object().value(u"path").toString(), QStringLiteral("Äsche am Bach.jpg"));
    QCOMPARE(root.object().value(u"album").toString(), QString());

    const Response png = env->request("POST", QStringLiteral("/api/media?album=alpen&name=bild.jpg"), pngBytes());
    QCOMPARE(png.status, 201);
    QVERIFY(png.object().value(u"path").toString().endsWith(QStringLiteral(".png")));

    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=text.jpg"), "Das ist kein Bild").status, 415);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=..%2Fx&name=a.jpg"), jpegBytes()).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=.versteckt&name=a.jpg"), jpegBytes()).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=leer.jpg")).status, 400);
    QVERIFY(!QFileInfo::exists(env->path(QStringLiteral("x"))));
}

void TestApi::uploadLimits()
{
    const auto env = makeEnv({{QStringLiteral("admin"), QJsonObject{{QStringLiteral("maxUploadMB"), 1}}}});
    QVERIFY(env);
    QVERIFY(env->login());

    QByteArray big = jpegBytes();
    big.append(QByteArray(1200 * 1024, 'x'));
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=gross.jpg"), big).status, 413);
    QVERIFY(filesBelow(env->media()).isEmpty());
}

void TestApi::mediaAndLibrary()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());

    // No library yet → an empty list, not an error
    Response list = env->get(QStringLiteral("/api/media"));
    QCOMPARE(list.status, 200);
    QCOMPARE(list.object().value(u"total").toInt(), 0);

    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=Elch.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=Fuchs.jpg"), jpegBytes()).status, 201);
    env->sync();

    list = env->get(QStringLiteral("/api/media"));
    QCOMPARE(list.status, 200);
    QCOMPARE(list.object().value(u"total").toInt(), 2);
    const QJsonArray items = list.object().value(u"items").toArray();
    QCOMPARE(items.size(), 2);
    for (const QJsonValue &item : items) {
        QVERIFY(item.toObject().contains(QStringLiteral("id")));
        QVERIFY(item.toObject().contains(QStringLiteral("album")));
        QCOMPARE(item.toObject().value(u"enabled").toBool(), true);
    }
    const qint64 elch = idOf(items, QStringLiteral("alpen/Elch.jpg"));
    const qint64 fuchs = idOf(items, QStringLiteral("Fuchs.jpg"));
    QVERIFY(elch > 0 && fuchs > 0);

    QCOMPARE(env->get(QStringLiteral("/api/media?album=alpen")).object().value(u"total").toInt(), 1);
    QCOMPARE(env->get(QStringLiteral("/api/media?limit=1")).object().value(u"items").toArray().size(), 1);

    Response one = env->get(QStringLiteral("/api/media/%1").arg(elch));
    QCOMPARE(one.status, 200);
    QVERIFY(!one.object().contains(QStringLiteral("title")));
    QCOMPARE(env->get(QStringLiteral("/api/media/999")).status, 404);

    // Edits go to the sidecar
    const Response patch = env->request("PATCH", QStringLiteral("/api/media/%1").arg(elch),
                                        jsonBytes({{QStringLiteral("title"), QStringLiteral("Elch")},
                                                   {QStringLiteral("enabled"), false}}),
                                        {{"Content-Type", "application/json"}});
    QCOMPARE(patch.status, 202);
    const QJsonObject sidecar = readJsonFile(env->media() + QStringLiteral("/alpen/Elch.json"));
    QCOMPARE(sidecar.value(u"title").toString(), QStringLiteral("Elch"));
    QCOMPARE(sidecar.value(u"enabled").toBool(true), false);

    QCOMPARE(env->request("PATCH", QStringLiteral("/api/media/%1").arg(elch), jsonBytes({{QStringLiteral("foo"), 1}})).status, 400);
    QCOMPARE(env->request("PATCH", QStringLiteral("/api/media/%1").arg(elch), jsonBytes({{QStringLiteral("title"), 5}})).status, 400);
    QCOMPARE(env->request("PATCH", QStringLiteral("/api/media/%1").arg(elch), "kein json").status, 400);
    QCOMPARE(env->request("PATCH", QStringLiteral("/api/media/999"), jsonBytes({{QStringLiteral("title"), QStringLiteral("x")}})).status, 404);

    env->sync();
    one = env->get(QStringLiteral("/api/media/%1").arg(elch));
    QCOMPARE(one.object().value(u"title").toString(), QStringLiteral("Elch"));
    QCOMPARE(one.object().value(u"enabled").toBool(), false);

    // Showing again removes the key, the sidecar stays small
    QCOMPARE(env->request("PATCH", QStringLiteral("/api/media/%1").arg(elch), jsonBytes({{QStringLiteral("enabled"), true}})).status, 202);
    QVERIFY(!readJsonFile(env->media() + QStringLiteral("/alpen/Elch.json")).contains(QStringLiteral("enabled")));

    // Files: original until renditions exist
    QCOMPARE(env->get(QStringLiteral("/api/media/%1/thumb").arg(fuchs)).status, 404);
    Response image = env->get(QStringLiteral("/api/media/%1/image").arg(fuchs));
    QCOMPARE(image.status, 200);
    QCOMPARE(image.header("Content-Type"), QByteArray("image/jpeg"));
    QFile original(env->media() + QStringLiteral("/Fuchs.jpg"));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(image.body, original.readAll());
    QCOMPARE(env->get(QStringLiteral("/api/media/%1/image?kind=1080p").arg(fuchs)).status, 404);
    {
        const LibrarySettings settings = Config::load(env->configPath).library();
        Library library(settings.database, env->media(), settings.cacheRoot);
        QVERIFY(library.open());
        for (const RenditionJob &job : library.pendingRenditions({QStringLiteral("thumb")}, 10))
            QVERIFY(library.generate(job, 85));
    }
    const Response thumb = env->get(QStringLiteral("/api/media/%1/thumb").arg(fuchs));
    QCOMPARE(thumb.status, 200);
    QCOMPARE(thumb.header("Content-Type"), QByteArray("image/jpeg"));
    QVERIFY(!QImage::fromData(thumb.body).isNull());

    // Delete moves picture and sidecar to the trash
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/media/%1").arg(elch), {}, {}, false).status, 403);
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/media/%1").arg(elch)).status, 204);
    QVERIFY(!QFile::exists(env->media() + QStringLiteral("/alpen/Elch.jpg")));
    QVERIFY(!QFile::exists(env->media() + QStringLiteral("/alpen/Elch.json")));
    const QStringList trash = filesBelow(env->path(QStringLiteral("trash")));
    QCOMPARE(trash.size(), 2);
    QVERIFY(trash.at(0).endsWith(QStringLiteral("/alpen/Elch.jpg")));
    QVERIFY(trash.at(1).endsWith(QStringLiteral("/alpen/Elch.json")));
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/media/999")).status, 404);

    env->sync();
    QCOMPARE(env->get(QStringLiteral("/api/media")).object().value(u"total").toInt(), 1);
}

void TestApi::albums()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=a.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=b.jpg"), jpegBytes()).status, 201);
    env->sync();

    const auto count = [](const QJsonArray &albums, const QString &name) {
        for (const QJsonValue &album : albums) {
            if (album.toObject().value(u"name").toString() == name)
                return album.toObject().value(u"count").toInt();
        }
        return -1;
    };

    QJsonArray albums = env->get(QStringLiteral("/api/albums")).array();
    QCOMPARE(count(albums, QString()), 0);
    QCOMPARE(count(albums, QStringLiteral("alpen")), 2);

    const Response created = env->post(QStringLiteral("/api/albums"), {{QStringLiteral("name"), QStringLiteral("reisen/norwegen")}});
    QCOMPARE(created.status, 201);
    QCOMPARE(created.object().value(u"name").toString(), QStringLiteral("reisen/norwegen"));
    QVERIFY(QFileInfo(env->media() + QStringLiteral("/reisen/norwegen")).isDir());

    QCOMPARE(env->post(QStringLiteral("/api/albums"), {{QStringLiteral("name"), QStringLiteral("../x")}}).status, 400);
    QCOMPARE(env->post(QStringLiteral("/api/albums"), {{QStringLiteral("name"), QString()}}).status, 400);
    QCOMPARE(env->post(QStringLiteral("/api/albums"), {{QStringLiteral("name"), QStringLiteral("@eaDir")}}).status, 400);

    albums = env->get(QStringLiteral("/api/albums")).array();
    QCOMPARE(count(albums, QStringLiteral("reisen/norwegen")), 0);
    QCOMPARE(count(albums, QStringLiteral("reisen")), 0);
}

void TestApi::config()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());

    const QJsonObject original = readJsonFile(env->configPath);
    QCOMPARE(env->get(QStringLiteral("/api/config")).object(), original);

    QJsonObject changed = original;
    changed.insert(QStringLiteral("locale"), QStringLiteral("en_GB"));
    changed.insert(QStringLiteral("layouts"), QJsonArray{QJsonObject{
        {QStringLiteral("id"), QStringLiteral("tag")},
        {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("slideshow")}}}},
    }});
    QCOMPARE(env->request("PUT", QStringLiteral("/api/config"), jsonBytes(changed)).status, 204);
    QCOMPARE(readJsonFile(env->configPath), changed);
    QCOMPARE(readJsonFile(env->configPath + QStringLiteral(".bak")), original);

    QCOMPARE(env->request("PUT", QStringLiteral("/api/config"), jsonBytes({{QStringLiteral("layouts"), QJsonObject()}})).status, 400);
    const QJsonObject noType{{QStringLiteral("layouts"), QJsonArray{QJsonObject{
        {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("x"), 0}}}},
    }}}};
    QCOMPARE(env->request("PUT", QStringLiteral("/api/config"), jsonBytes(noType)).status, 400);
    QCOMPARE(env->request("PUT", QStringLiteral("/api/config"), "{ kaputt").status, 400);
    QCOMPARE(env->request("PUT", QStringLiteral("/api/config"), jsonBytes(changed), {}, false).status, 403);
    QCOMPARE(readJsonFile(env->configPath), changed);
}

void TestApi::displayOffline()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());

    const Response next = env->request("POST", QStringLiteral("/api/display/next"));
    QCOMPARE(next.status, 503);
    QVERIFY(!next.object().value(u"error").toString().isEmpty());
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/pause")).status, 503);
    QCOMPARE(env->get(QStringLiteral("/api/display/screenshot")).status, 503);

    const Response status = env->get(QStringLiteral("/api/status"));
    QCOMPARE(status.status, 200);
    QCOMPARE(status.object().value(u"display").toObject().value(u"connected").toBool(true), false);
    QVERIFY(status.object().value(u"storage").toObject().value(u"total").toDouble() > 0);
    QVERIFY(!status.object().value(u"version").toString().isEmpty());
}

void TestApi::previewLayout()
{
    const auto env = makeEnv();
    QVERIFY(env);
    const QJsonObject layout{
        {QStringLiteral("id"), QStringLiteral("probe")},
        {QStringLiteral("grid"), QJsonObject{{QStringLiteral("columns"), 12}, {QStringLiteral("rows"), 12}}},
        {QStringLiteral("regions"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("main")}, {QStringLiteral("type"), QStringLiteral("slideshow")},
             {QStringLiteral("x"), 0}, {QStringLiteral("y"), 0}, {QStringLiteral("w"), 12}, {QStringLiteral("h"), 12}}}},
    };
    const auto body = [](const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); };
    const QByteArray valid = body({{QStringLiteral("layout"), layout}, {QStringLiteral("seconds"), 30}});

    // No session yet
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/preview"), valid).status, 401);

    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/preview"), valid, {}, false).status, 403);
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/preview"), body({})).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/preview"),
                          body({{QStringLiteral("layout"), QStringLiteral("vollbild")}})).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/preview"), QByteArray("{kaputt")).status, 400);

    QJsonObject untyped = layout;
    untyped.insert(QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("x")}}});
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/preview"),
                          body({{QStringLiteral("layout"), untyped}})).status, 400);

    // Valid, but no display to show it on
    const Response offline = env->request("POST", QStringLiteral("/api/display/preview"), valid);
    QCOMPARE(offline.status, 503);
    QVERIFY(!offline.object().value(u"error").toString().isEmpty());

    QCOMPARE(env->request("DELETE", QStringLiteral("/api/display/preview"), {}, {}, false).status, 403);
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/display/preview")).status, 503);
}

void TestApi::screenSwitch()
{
    const auto env = makeEnv();
    QVERIFY(env);
    const QByteArray on = R"({"on": true})";

    QCOMPARE(env->request("POST", QStringLiteral("/api/display/screen"), on).status, 401);
    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/screen"), on, {}, false).status, 403);
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/screen"), QByteArray("{}")).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/screen"), QByteArray(R"({"on": "ja"})")).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/display/screen"), QByteArray("{kaputt")).status, 400);

    // Valid, but no display connected
    const Response offline = env->request("POST", QStringLiteral("/api/display/screen"), QByteArray(R"({"on": false})"));
    QCOMPARE(offline.status, 503);
    QVERIFY(!offline.object().value(u"error").toString().isEmpty());
}

void TestApi::publicProfileLogin()
{
    // The public profile only shortens sessions; logging in still works
    const auto env = makeEnv(QJsonObject{{QStringLiteral("profile"), QStringLiteral("public")}});
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 200);
}

void TestApi::mediaFilters()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());

    for (const char *name : {"anna.jpg", "berta.jpg", "carla.jpg", "dora.jpg"})
        QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=wald&name=%1").arg(QLatin1String(name)), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=Fuchs.jpg"), jpegBytes()).status, 201);

    // % and _ in titles must be matched literally
    QVERIFY(writeJsonFile(env->media() + QStringLiteral("/wald/anna.json"),
                          {{QStringLiteral("title"), QStringLiteral("100% Natur")}, {QStringLiteral("location"), QStringLiteral("Eifel")}}));
    QVERIFY(writeJsonFile(env->media() + QStringLiteral("/wald/berta.json"),
                          {{QStringLiteral("title"), QStringLiteral("1000 Natur")}, {QStringLiteral("enabled"), false}}));
    QVERIFY(writeJsonFile(env->media() + QStringLiteral("/wald/carla.json"),
                          {{QStringLiteral("title"), QStringLiteral("a_b")}, {QStringLiteral("description"), QStringLiteral("Morgennebel")}}));
    QVERIFY(writeJsonFile(env->media() + QStringLiteral("/wald/dora.json"), {{QStringLiteral("title"), QStringLiteral("axb")}}));
    setTaken(env->media() + QStringLiteral("/wald/anna.jpg"), "2020:05:01 06:00:00");
    setTaken(env->media() + QStringLiteral("/wald/carla.jpg"), "2022:09:12 18:30:00");
    env->sync();

    // New fields: size on disk and a version that follows the file
    QJsonArray items = env->get(QStringLiteral("/api/media")).object().value(u"items").toArray();
    QCOMPARE(items.size(), 5);
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QFileInfo file(env->media() + u'/' + item.value(u"path").toString());
        QCOMPARE(item.value(u"fileSize").toInteger(), file.size());
        QCOMPARE(item.value(u"version").toInteger(), file.lastModified().toMSecsSinceEpoch());
    }

    const auto search = [&](const QString &query) {
        QStringList paths = pathsOf(env->get(QStringLiteral("/api/media?") + query).object().value(u"items").toArray());
        paths.sort();
        return paths;
    };
    QCOMPARE(search(QStringLiteral("q=100%25")), QStringList{QStringLiteral("wald/anna.jpg")});
    QCOMPARE(search(QStringLiteral("q=a_b")), QStringList{QStringLiteral("wald/carla.jpg")});
    QCOMPARE(search(QStringLiteral("q=eifel")), QStringList{QStringLiteral("wald/anna.jpg")});       // location, any case
    QCOMPARE(search(QStringLiteral("q=Morgennebel")), QStringList{QStringLiteral("wald/carla.jpg")}); // description
    QCOMPARE(search(QStringLiteral("q=fuchs")), QStringList{QStringLiteral("Fuchs.jpg")});            // path
    QCOMPARE(search(QStringLiteral("q=natur&album=wald")),
             (QStringList{QStringLiteral("wald/anna.jpg"), QStringLiteral("wald/berta.jpg")}));
    QCOMPARE(env->get(QStringLiteral("/api/media?q=natur")).object().value(u"total").toInt(), 2);

    QCOMPARE(search(QStringLiteral("enabled=false")), QStringList{QStringLiteral("wald/berta.jpg")});
    QCOMPARE(search(QStringLiteral("enabled=true")).size(), 4);
    QCOMPARE(env->get(QStringLiteral("/api/media?enabled=false")).object().value(u"total").toInt(), 1);
    QCOMPARE(search(QStringLiteral("enabled=quatsch")).size(), 5); // ignored

    const auto ordered = [&](const QString &query) {
        return pathsOf(env->get(QStringLiteral("/api/media?") + query).object().value(u"items").toArray());
    };
    const QStringList byName{QStringLiteral("Fuchs.jpg"), QStringLiteral("wald/anna.jpg"), QStringLiteral("wald/berta.jpg"),
                             QStringLiteral("wald/carla.jpg"), QStringLiteral("wald/dora.jpg")};
    QCOMPARE(ordered(QStringLiteral("sort=name&order=asc")), byName);
    QStringList reversed = byName;
    std::reverse(reversed.begin(), reversed.end());
    QCOMPARE(ordered(QStringLiteral("sort=name")), reversed); // descending by default

    // Undated pictures come last, whatever the direction
    QStringList takenDesc = ordered(QStringLiteral("sort=taken"));
    QCOMPARE(takenDesc.mid(0, 2), (QStringList{QStringLiteral("wald/carla.jpg"), QStringLiteral("wald/anna.jpg")}));
    QStringList takenAsc = ordered(QStringLiteral("sort=taken&order=asc"));
    QCOMPARE(takenAsc.mid(0, 2), (QStringList{QStringLiteral("wald/anna.jpg"), QStringLiteral("wald/carla.jpg")}));
    QCOMPARE(takenDesc.size(), 5);
    QCOMPARE(takenAsc.size(), 5);

    // Paging over a sorted list
    QCOMPARE(ordered(QStringLiteral("sort=name&order=asc&offset=1&limit=2")), byName.mid(1, 2));
}

void TestApi::albumCovers()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=a.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=b.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->post(QStringLiteral("/api/albums"), {{QStringLiteral("name"), QStringLiteral("leer")}}).status, 201);
    env->sync();

    const QJsonArray media = env->get(QStringLiteral("/api/media?album=alpen")).object().value(u"items").toArray();
    qint64 newest = 0;
    qint64 newestVersion = 0;
    for (const QJsonValue &item : media) {
        if (item.toObject().value(u"id").toInteger() > newest) {
            newest = item.toObject().value(u"id").toInteger();
            newestVersion = item.toObject().value(u"version").toInteger();
        }
    }

    QJsonObject alpen;
    QJsonObject leer;
    for (const QJsonValue &album : env->get(QStringLiteral("/api/albums")).array()) {
        if (album.toObject().value(u"name").toString() == u"alpen")
            alpen = album.toObject();
        if (album.toObject().value(u"name").toString() == u"leer")
            leer = album.toObject();
    }
    QCOMPARE(alpen.value(u"cover").toObject().value(u"id").toInteger(), newest);
    QCOMPARE(alpen.value(u"cover").toObject().value(u"version").toInteger(), newestVersion);
    QVERIFY(!leer.isEmpty());
    QVERIFY(!leer.contains(QStringLiteral("cover")));
}

void TestApi::batch()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    for (const char *name : {"a.jpg", "b.jpg", "c.jpg"})
        QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=%1").arg(QLatin1String(name)), jpegBytes()).status, 201);
    QVERIFY(writeJsonFile(env->media() + QStringLiteral("/alpen/b.json"), {{QStringLiteral("title"), QStringLiteral("Bergsee")}}));
    env->sync();
    const QJsonArray items = env->get(QStringLiteral("/api/media")).object().value(u"items").toArray();
    const qint64 a = idOf(items, QStringLiteral("alpen/a.jpg"));
    const qint64 b = idOf(items, QStringLiteral("alpen/b.jpg"));
    const qint64 c = idOf(items, QStringLiteral("alpen/c.jpg"));
    QSignalSpy sync(env->server.get(), &ApiServer::syncRequested);

    const auto run = [&](const QJsonObject &body) {
        return env->request("POST", QStringLiteral("/api/media/batch"), jsonBytes(body), {{"Content-Type", "application/json"}});
    };

    // Hide two, one unknown id
    Response result = run({{QStringLiteral("ids"), QJsonArray{a, b, 999}}, {QStringLiteral("action"), QStringLiteral("disable")}});
    QCOMPARE(result.status, 200);
    QCOMPARE(result.object().value(u"done").toInt(), 2);
    const QJsonArray failed = result.object().value(u"failed").toArray();
    QCOMPARE(failed.size(), 1);
    QCOMPARE(failed.at(0).toObject().value(u"id").toInteger(), 999);
    QVERIFY(!failed.at(0).toObject().value(u"error").toString().isEmpty());
    QCOMPARE(sync.count(), 1);
    QCOMPARE(readJsonFile(env->media() + QStringLiteral("/alpen/a.json")).value(u"enabled").toBool(true), false);
    QCOMPARE(readJsonFile(env->media() + QStringLiteral("/alpen/b.json")).value(u"title").toString(), QStringLiteral("Bergsee"));
    env->sync();
    QCOMPARE(env->get(QStringLiteral("/api/media?enabled=false")).object().value(u"total").toInt(), 2);

    // Showing again removes the key; a sidecar that only held it goes away
    result = run({{QStringLiteral("ids"), QJsonArray{a, b}}, {QStringLiteral("action"), QStringLiteral("enable")}});
    QCOMPARE(result.object().value(u"done").toInt(), 2);
    QVERIFY(!QFileInfo::exists(env->media() + QStringLiteral("/alpen/a.json")));
    QCOMPARE(readJsonFile(env->media() + QStringLiteral("/alpen/b.json")),
             (QJsonObject{{QStringLiteral("title"), QStringLiteral("Bergsee")}}));

    // Move: file and sidecar, modification time kept, the library keeps the ids
    const QDateTime modified = QFileInfo(env->media() + QStringLiteral("/alpen/b.jpg")).lastModified();
    result = run({{QStringLiteral("ids"), QJsonArray{b}}, {QStringLiteral("action"), QStringLiteral("move")},
                  {QStringLiteral("album"), QStringLiteral("reisen/see")}});
    QCOMPARE(result.status, 200);
    QCOMPARE(result.object().value(u"done").toInt(), 1);
    QVERIFY(!QFileInfo::exists(env->media() + QStringLiteral("/alpen/b.jpg")));
    QVERIFY(QFileInfo::exists(env->media() + QStringLiteral("/reisen/see/b.jpg")));
    QVERIFY(QFileInfo::exists(env->media() + QStringLiteral("/reisen/see/b.json")));
    QCOMPARE(QFileInfo(env->media() + QStringLiteral("/reisen/see/b.jpg")).lastModified(), modified);
    env->sync();
    const QJsonObject moved = env->get(QStringLiteral("/api/media/%1").arg(b)).object();
    QCOMPARE(moved.value(u"path").toString(), QStringLiteral("reisen/see/b.jpg"));
    QCOMPARE(moved.value(u"album").toString(), QStringLiteral("reisen/see"));
    QCOMPARE(moved.value(u"title").toString(), QStringLiteral("Bergsee"));

    // Into the root folder
    result = run({{QStringLiteral("ids"), QJsonArray{c}}, {QStringLiteral("action"), QStringLiteral("move")},
                  {QStringLiteral("album"), QString()}});
    QCOMPARE(result.object().value(u"done").toInt(), 1);
    QVERIFY(QFileInfo::exists(env->media() + QStringLiteral("/c.jpg")));

    // Delete goes to the trash
    result = run({{QStringLiteral("ids"), QJsonArray{a}}, {QStringLiteral("action"), QStringLiteral("delete")}});
    QCOMPARE(result.object().value(u"done").toInt(), 1);
    QVERIFY(!QFileInfo::exists(env->media() + QStringLiteral("/alpen/a.jpg")));
    QCOMPARE(filesBelow(env->path(QStringLiteral("trash"))).size(), 1);
    env->sync();
    QCOMPARE(env->get(QStringLiteral("/api/media")).object().value(u"total").toInt(), 2);
    QCOMPARE(env->get(QStringLiteral("/api/media/%1").arg(c)).object().value(u"path").toString(), QStringLiteral("c.jpg"));
}

void TestApi::batchErrors()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QSignalSpy sync(env->server.get(), &ApiServer::syncRequested);
    const auto run = [&](const QByteArray &body, bool csrf = true) {
        return env->request("POST", QStringLiteral("/api/media/batch"), body, {{"Content-Type", "application/json"}}, csrf).status;
    };

    QCOMPARE(run(jsonBytes({{QStringLiteral("ids"), QJsonArray{1}}, {QStringLiteral("action"), QStringLiteral("enable")}}), false), 403);
    QCOMPARE(run(jsonBytes({{QStringLiteral("ids"), QJsonArray{1}}, {QStringLiteral("action"), QStringLiteral("explode")}})), 400);
    QCOMPARE(run(jsonBytes({{QStringLiteral("action"), QStringLiteral("enable")}})), 400);
    QCOMPARE(run(jsonBytes({{QStringLiteral("ids"), 5}, {QStringLiteral("action"), QStringLiteral("enable")}})), 400);
    QCOMPARE(run("kein json"), 400);
    QCOMPARE(run(jsonBytes({{QStringLiteral("ids"), QJsonArray{1}}, {QStringLiteral("action"), QStringLiteral("move")},
                            {QStringLiteral("album"), QStringLiteral("../raus")}})), 400);
    QCOMPARE(run(jsonBytes({{QStringLiteral("ids"), QJsonArray{1}}, {QStringLiteral("action"), QStringLiteral("move")},
                            {QStringLiteral("album"), QStringLiteral(".versteckt")}})), 400);

    QJsonArray many;
    for (int i = 0; i < 1001; ++i)
        many.append(i);
    QCOMPARE(run(jsonBytes({{QStringLiteral("ids"), many}, {QStringLiteral("action"), QStringLiteral("enable")}})), 400);

    // Nothing done, nothing to sync
    const Response none = env->request("POST", QStringLiteral("/api/media/batch"),
                                       jsonBytes({{QStringLiteral("ids"), QJsonArray{42}}, {QStringLiteral("action"), QStringLiteral("delete")}}));
    QCOMPARE(none.status, 200);
    QCOMPARE(none.object().value(u"done").toInt(), 0);
    QCOMPARE(none.object().value(u"failed").toArray().size(), 1);
    QCOMPARE(sync.count(), 0);
}

void TestApi::renameAlbum()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=a.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen/hoch&name=b.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->post(QStringLiteral("/api/albums"), {{QStringLiteral("name"), QStringLiteral("wald")}}).status, 201);
    env->sync();
    const QJsonArray before = env->get(QStringLiteral("/api/media")).object().value(u"items").toArray();
    const qint64 a = idOf(before, QStringLiteral("alpen/a.jpg"));
    const qint64 b = idOf(before, QStringLiteral("alpen/hoch/b.jpg"));
    QSignalSpy sync(env->server.get(), &ApiServer::syncRequested);

    const auto rename = [&](const QString &from, const QString &to, bool csrf = true) {
        return env->request("PATCH", QStringLiteral("/api/albums"),
                            jsonBytes({{QStringLiteral("from"), from}, {QStringLiteral("to"), to}}),
                            {{"Content-Type", "application/json"}}, csrf);
    };

    QCOMPARE(rename(QStringLiteral("alpen"), QStringLiteral("berge"), false).status, 403);
    QCOMPARE(rename(QStringLiteral("alpen"), QStringLiteral("wald")).status, 409);
    QCOMPARE(rename(QStringLiteral("gibt-es-nicht"), QStringLiteral("neu")).status, 404);
    QCOMPARE(rename(QString(), QStringLiteral("neu")).status, 400);
    QCOMPARE(rename(QStringLiteral("alpen"), QString()).status, 400);
    QCOMPARE(rename(QStringLiteral("alpen"), QStringLiteral("../raus")).status, 400);
    QCOMPARE(rename(QStringLiteral("alpen"), QStringLiteral("alpen/drinnen")).status, 400);
    QCOMPARE(env->request("PATCH", QStringLiteral("/api/albums"), "kein json").status, 400);
    QCOMPARE(sync.count(), 0);

    // Renaming may also move the album below another one
    const Response renamed = rename(QStringLiteral("alpen"), QStringLiteral("reisen/berge"));
    QCOMPARE(renamed.status, 200);
    QCOMPARE(renamed.object().value(u"name").toString(), QStringLiteral("reisen/berge"));
    QCOMPARE(sync.count(), 1);
    QVERIFY(!QFileInfo::exists(env->media() + QStringLiteral("/alpen")));
    QVERIFY(QFileInfo::exists(env->media() + QStringLiteral("/reisen/berge/hoch/b.jpg")));

    // The library sees the moves: same ids, new paths
    env->sync();
    QCOMPARE(env->get(QStringLiteral("/api/media/%1").arg(a)).object().value(u"path").toString(), QStringLiteral("reisen/berge/a.jpg"));
    QCOMPARE(env->get(QStringLiteral("/api/media/%1").arg(b)).object().value(u"album").toString(), QStringLiteral("reisen/berge/hoch"));
    QCOMPARE(env->get(QStringLiteral("/api/media")).object().value(u"total").toInt(), 2);
}

void TestApi::deleteAlbum()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen&name=a.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?album=alpen/hoch&name=b.jpg"), jpegBytes()).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=bleibt.jpg"), jpegBytes()).status, 201);
    env->sync();
    QSignalSpy sync(env->server.get(), &ApiServer::syncRequested);

    QCOMPARE(env->request("DELETE", QStringLiteral("/api/albums?name=alpen"), {}, {}, false).status, 403);
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/albums?name=")).status, 400);
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/albums")).status, 400);
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/albums?name=..%2F..")).status, 400);
    QCOMPARE(env->request("DELETE", QStringLiteral("/api/albums?name=gibt-es-nicht")).status, 404);
    QCOMPARE(sync.count(), 0);

    QCOMPARE(env->request("DELETE", QStringLiteral("/api/albums?name=alpen")).status, 204);
    QCOMPARE(sync.count(), 1);
    QVERIFY(!QFileInfo::exists(env->media() + QStringLiteral("/alpen")));
    QVERIFY(QFileInfo::exists(env->media() + QStringLiteral("/bleibt.jpg")));

    // trash/<stamp>/alpen/… with everything that was in it
    const QStringList trash = filesBelow(env->path(QStringLiteral("trash")));
    QCOMPARE(trash.size(), 2);
    QVERIFY(trash.at(0).endsWith(QStringLiteral("/alpen/a.jpg")));
    QVERIFY(trash.at(1).endsWith(QStringLiteral("/alpen/hoch/b.jpg")));

    env->sync();
    QCOMPARE(env->get(QStringLiteral("/api/media")).object().value(u"total").toInt(), 1);
}

void TestApi::changePassword()
{
    const auto env = makeEnv();
    QVERIFY(env);
    const Response login = env->post(QStringLiteral("/api/auth/login"),
                                     {{QStringLiteral("username"), QStringLiteral("admin")},
                                      {QStringLiteral("password"), QStringLiteral("geheim123")}});
    QCOMPARE(login.status, 200);
    const QByteArray oldCookie = login.header("Set-Cookie").split(';').constFirst();

    const auto change = [&](const QString &current, const QString &next, bool csrf = true) {
        return env->request("POST", QStringLiteral("/api/auth/password"),
                            jsonBytes({{QStringLiteral("current"), current}, {QStringLiteral("new"), next}}),
                            {{"Content-Type", "application/json"}}, csrf);
    };

    QCOMPARE(change(QStringLiteral("geheim123"), QStringLiteral("neuesPasswort"), false).status, 403);
    QCOMPARE(change(QStringLiteral("falsch"), QStringLiteral("neuesPasswort")).status, 403);
    QCOMPARE(change(QStringLiteral("geheim123"), QStringLiteral("kurz")).status, 400);
    QCOMPARE(env->request("POST", QStringLiteral("/api/auth/password"), "kein json").status, 400);
    QVERIFY(env->login()); // nothing changed so far
    // (logging in again replaced the cookie; the old one is still a valid session)

    const Response changed = change(QStringLiteral("geheim123"), QStringLiteral("neuesPasswort"));
    QCOMPARE(changed.status, 204);
    const QByteArray cleared = changed.header("Set-Cookie");
    QVERIFY2(cleared.startsWith("berry_session=;"), cleared.constData());
    QVERIFY(cleared.contains("Max-Age=0"));

    // Every session has ended, including ones the browser did not drop
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 401);
    QCOMPARE(env->request("GET", QStringLiteral("/api/status"), {}, {{"Cookie", oldCookie}}).status, 401);
    QVERIFY(!env->login());
    QVERIFY(env->login(QStringLiteral("neuesPasswort")));
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 200);

    // Not logged in: no password change
    QCOMPARE(env->request("POST", QStringLiteral("/api/auth/logout")).status, 204);
    QCOMPARE(change(QStringLiteral("neuesPasswort"), QStringLiteral("nochEinPasswort")).status, 401);
    QVERIFY(env->login(QStringLiteral("neuesPasswort")));
}

void TestApi::hostHeader()
{
    const auto env = makeEnv({{QStringLiteral("admin"),
                               QJsonObject{{QStringLiteral("allowedHosts"), QJsonArray{QStringLiteral("kiosk.example")}}}}});
    QVERIFY(env);
    QVERIFY(env->login());
    const quint16 port = env->server->serverPort();
    const QByteArray cookie = sessionCookie(env->nam, port);
    QVERIFY(!cookie.isEmpty());
    const QByteArray self = "127.0.0.1:" + QByteArray::number(port);

    // DNS rebinding: a foreign page's name as Host is refused everywhere
    QCOMPARE(rawStatus(port, "GET", "/api/auth/session", "evil.example"), 403);
    QCOMPARE(rawStatus(port, "GET", "/api/status", "evil.example", {cookie}), 403);
    QCOMPARE(rawStatus(port, "GET", "/api/status", "kiosk.example.evil.example", {cookie}), 403);
    const QByteArray login = jsonBytes({{QStringLiteral("username"), QStringLiteral("admin")},
                                        {QStringLiteral("password"), QStringLiteral("geheim123")}});
    QCOMPARE(rawStatus(port, "POST", "/api/auth/login", "evil.example",
                       {"X-Berry-Kiosk: 1", "Content-Type: application/json"}, login),
             403);

    // IP addresses, localhost and configured names are fine
    QCOMPARE(rawStatus(port, "GET", "/api/auth/session", self), 200);
    QCOMPARE(rawStatus(port, "GET", "/api/status", self, {cookie}), 200);
    QCOMPARE(rawStatus(port, "GET", "/api/status", "localhost:" + QByteArray::number(port), {cookie}), 200);
    QCOMPARE(rawStatus(port, "GET", "/api/status", "[::1]:" + QByteArray::number(port), {cookie}), 200);
    QCOMPARE(rawStatus(port, "GET", "/api/status", "kiosk.example", {cookie}), 200);
    QCOMPARE(rawStatus(port, "GET", "/api/status", "KIOSK.Example.:8080", {cookie}), 200);
    QCOMPARE(rawStatus(port, "POST", "/api/auth/login", "kiosk.example",
                       {"X-Berry-Kiosk: 1", "Content-Type: application/json"}, login),
             200);
}

void TestApi::loginNeedsCsrfHeader()
{
    const auto env = makeEnv();
    QVERIFY(env);
    const auto attempt = [&](const QString &password, bool csrf, const QList<QPair<QByteArray, QByteArray>> &extra = {}) {
        QList<QPair<QByteArray, QByteArray>> headers{{"Content-Type", "application/json"}};
        headers += extra;
        return env->request("POST", QStringLiteral("/api/auth/login"),
                            jsonBytes({{QStringLiteral("username"), QStringLiteral("admin")},
                                       {QStringLiteral("password"), password}}),
                            headers, csrf)
            .status;
    };

    // Cross-site form posts never reach the password check, so they cannot
    // use up the address's attempts and lock the admin out
    for (int i = 0; i < 6; ++i)
        QCOMPARE(attempt(QStringLiteral("falsch"), false), 403);
    QCOMPARE(attempt(QStringLiteral("geheim123"), false), 403);
    QCOMPARE(attempt(QStringLiteral("geheim123"), true, {{"Origin", "http://evil.example"}}), 403);
    QCOMPARE(env->get(QStringLiteral("/api/status")).status, 401);
    QVERIFY(env->login());
}

void TestApi::changePasswordThrottle()
{
    const auto change = [](Env &env, const QString &current) {
        return env.request("POST", QStringLiteral("/api/auth/password"),
                           jsonBytes({{QStringLiteral("current"), current}, {QStringLiteral("new"), QStringLiteral("neuesPasswort")}}),
                           {{"Content-Type", "application/json"}})
            .status;
    };

    // Guessing the current password is braked like the login
    {
        const auto env = makeEnv();
        QVERIFY(env);
        QVERIFY(env->login());
        for (int i = 0; i < 5; ++i)
            QCOMPARE(change(*env, QStringLiteral("falsch")), 403);
        QCOMPARE(change(*env, QStringLiteral("falsch")), 429);
        QCOMPARE(change(*env, QStringLiteral("geheim123")), 429); // even the right one has to wait
        QVERIFY(!env->login());
    }

    // A correct change clears the address's failures
    {
        const auto env = makeEnv();
        QVERIFY(env);
        QVERIFY(env->login());
        for (int i = 0; i < 4; ++i)
            QCOMPARE(change(*env, QStringLiteral("falsch")), 403);
        QCOMPARE(change(*env, QStringLiteral("geheim123")), 204);
        QVERIFY(env->login(QStringLiteral("neuesPasswort")));
        // Without the reset these would be failures 5 to 8: the 6th total gets 429
        for (int i = 0; i < 4; ++i)
            QCOMPARE(change(*env, QStringLiteral("falsch")), 403);
    }
}

void TestApi::imageKindAllowlist()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=a.jpg"), jpegBytes(), {{"Content-Type", "image/jpeg"}}).status, 201);
    env->sync();
    const qint64 id = idOf(env->get(QStringLiteral("/api/media")).object().value(u"items").toArray(), QStringLiteral("a.jpg"));
    QVERIFY(id > 0);
    const auto image = [&](const QString &kind) {
        return env->get(QStringLiteral("/api/media/%1/image?kind=%2").arg(id).arg(kind)).status;
    };

    QCOMPARE(image(QStringLiteral("..%2F..%2Fx")), 400);
    QCOMPARE(image(QStringLiteral("..%2F..%2F..%2Fetc")), 400);
    QCOMPARE(image(QStringLiteral("%2Fetc")), 400);
    QCOMPARE(image(QStringLiteral("foo")), 400);
    // Known kinds pass the check (not rendered yet: 404), the original is there
    QCOMPARE(image(QStringLiteral("thumb")), 404);
    QCOMPARE(image(QStringLiteral("1080p")), 404);
    QCOMPARE(image(QStringLiteral("2160p")), 404);
    QCOMPARE(image(QStringLiteral("original")), 200);
    QCOMPARE(env->get(QStringLiteral("/api/media/%1/image").arg(id)).status, 200);
}

void TestApi::configPathsLocked()
{
    const QJsonObject library{
        {QStringLiteral("database"), QStringLiteral("library.db")},
        {QStringLiteral("cache"), QStringLiteral("cache")},
        {QStringLiteral("state"), QStringLiteral("state")},
        {QStringLiteral("trash"), QStringLiteral("trash")},
    };
    const auto env = makeEnv({{QStringLiteral("library"), library}});
    QVERIFY(env);
    QVERIFY(env->login());

    const auto fileBytes = [&] {
        QFile file(env->configPath);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto put = [&](const QJsonObject &config) {
        return env->request("PUT", QStringLiteral("/api/config"), jsonBytes(config), {{"Content-Type", "application/json"}});
    };
    const QJsonObject original = readJsonFile(env->configPath);
    const QByteArray before = fileBytes();

    // Where data lives can only be changed on the device itself
    const auto changed = [&](const QString &section, const QString &key, const QJsonValue &value) {
        QJsonObject config = original;
        QJsonObject part = config.value(section).toObject();
        part.insert(key, value);
        config.insert(section, part);
        return config;
    };
    for (const auto &[section, key, value] : {
             std::tuple{QStringLiteral("media"), QStringLiteral("path"), QStringLiteral("/")},
             std::tuple{QStringLiteral("library"), QStringLiteral("state"), QStringLiteral("/tmp/anderswo")},
             std::tuple{QStringLiteral("library"), QStringLiteral("cache"), QStringLiteral("..")},
             std::tuple{QStringLiteral("library"), QStringLiteral("database"), QStringLiteral("/tmp/x.db")},
             std::tuple{QStringLiteral("library"), QStringLiteral("trash"), QStringLiteral("/")},
         }) {
        const Response response = put(changed(section, key, value));
        QCOMPARE(response.status, 400);
        QVERIFY(response.object().value(u"error").toString().contains(QStringLiteral("%1.%2").arg(section, key)));
        QCOMPARE(fileBytes(), before);
    }

    // Leaving the keys out keeps the values on disk
    QCOMPARE(put(QJsonObject{{QStringLiteral("locale"), QStringLiteral("en_GB")}}).status, 204);
    QJsonObject written = readJsonFile(env->configPath);
    QCOMPARE(written.value(u"locale").toString(), QStringLiteral("en_GB"));
    QCOMPARE(written.value(u"media").toObject().value(u"path").toString(), QStringLiteral("media"));
    QCOMPARE(written.value(u"library").toObject(), library);

    // A library object with only some keys gets the others back as well
    QCOMPARE(put(QJsonObject{{QStringLiteral("library"), QJsonObject{{QStringLiteral("jpegQuality"), 90}}}}).status, 204);
    written = readJsonFile(env->configPath);
    QCOMPARE(written.value(u"library").toObject().value(u"state").toString(), QStringLiteral("state"));
    QCOMPARE(written.value(u"library").toObject().value(u"jpegQuality").toInt(), 90);
    QCOMPARE(written.value(u"media").toObject().value(u"path").toString(), QStringLiteral("media"));

    // The same values are fine
    QJsonObject same = original;
    same.insert(QStringLiteral("quotes"), QJsonArray{QStringLiteral("Ein Zitat")});
    QCOMPARE(put(same).status, 204);

    // Adding a path the file does not set yet is a change too
    const auto plain = makeEnv();
    QVERIFY(plain);
    QVERIFY(plain->login());
    QCOMPARE(plain->request("PUT", QStringLiteral("/api/config"),
                            jsonBytes({{QStringLiteral("library"), QJsonObject{{QStringLiteral("cache"), QStringLiteral("/tmp")}}}}),
                            {{"Content-Type", "application/json"}})
                 .status,
             400);
}

void TestApi::uploadPixelLimit()
{
    const auto env = makeEnv();
    QVERIFY(env);
    QVERIFY(env->login());

    // Sanity: the crafted header really reads as a 20000 × 20000 PNG
    QByteArray bomb = pngHeaderOnly(20000, 20000);
    QBuffer buffer(&bomb);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    QCOMPARE(QImageReader::imageFormat(&buffer), QByteArray("png"));
    buffer.seek(0);
    QCOMPARE(QImageReader(&buffer, "png").size(), QSize(20000, 20000));

    const Response refused = env->request("POST", QStringLiteral("/api/media?name=bombe.png"), bomb, {{"Content-Type", "image/png"}});
    QCOMPARE(refused.status, 413);
    QVERIFY(refused.object().value(u"error").toString().contains(QStringLiteral("20000")));
    QVERIFY(!QFileInfo::exists(env->media() + QStringLiteral("/bombe.png")));

    // Large but plausible PNGs and ordinary JPEGs still go through
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=ok.png"), pngHeaderOnly(7000, 5000), {{"Content-Type", "image/png"}}).status, 201);
    QCOMPARE(env->request("POST", QStringLiteral("/api/media?name=foto.jpg"), jpegBytes(), {{"Content-Type", "image/jpeg"}}).status, 201);
}

void TestApi::credentialsFixedLocation()
{
    // config.json may name another state directory: the password still lives
    // next to config.json, where makeEnv() put it
    const auto env = makeEnv({{QStringLiteral("library"), QJsonObject{{QStringLiteral("state"), QStringLiteral("anderswo")}}}});
    QVERIFY(env);
    QVERIFY(saveCredentials(credentialsPath(env->path(QStringLiteral("anderswo"))),
                            Credentials::create(QStringLiteral("admin"), QStringLiteral("untergeschoben"), 1000)));

    QCOMPARE(env->get(QStringLiteral("/api/auth/session")).object().value(u"passwordSet").toBool(), true);
    QVERIFY(!env->login(QStringLiteral("untergeschoben")));
    QVERIFY(env->login());

    // A change lands in the fixed place as well
    QCOMPARE(env->request("POST", QStringLiteral("/api/auth/password"),
                          jsonBytes({{QStringLiteral("current"), QStringLiteral("geheim123")},
                                     {QStringLiteral("new"), QStringLiteral("neuesPasswort")}}),
                          {{"Content-Type", "application/json"}})
                 .status,
             204);
    QVERIFY(loadCredentials(credentialsPathForConfig(env->configPath)).verify(QStringLiteral("admin"), QStringLiteral("neuesPasswort")));
    QVERIFY(loadCredentials(credentialsPath(env->path(QStringLiteral("anderswo"))))
                .verify(QStringLiteral("admin"), QStringLiteral("untergeschoben")));
}

QTEST_GUILESS_MAIN(TestApi)
#include "tst_api.moc"
