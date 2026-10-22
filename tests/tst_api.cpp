#include "apiserver.h"
#include "auth.h"
#include "displaylink.h"
#include "imagemetadata.h"
#include "library.h"
#include "state.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

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

qint64 idOf(const QJsonArray &items, const QString &path)
{
    for (const QJsonValue &item : items) {
        if (item.toObject().value(u"path").toString() == path)
            return item.toObject().value(u"id").toInteger();
    }
    return 0;
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

    const Response script = env->get(QStringLiteral("/app.js"));
    QCOMPARE(script.status, 200);
    QVERIFY(script.header("Content-Type").startsWith("text/javascript"));
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

QTEST_GUILESS_MAIN(TestApi)
#include "tst_api.moc"
