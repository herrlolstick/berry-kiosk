#include "auth.h"
#include "config.h"
#include "displaylink.h"
#include "displayprotocol.h"
#include "imagemetadata.h"
#include "mediafiles.h"

#include <QBuffer>
#include <QCborMap>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>

using namespace berry;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

} // namespace

// Stands in for the display: lives in its own thread, because
// DisplayLink::request() blocks the thread it is called from.
class FakeDisplay : public QObject
{
    Q_OBJECT

public:
    std::atomic<bool> answer{true};

    FakeDisplay()
        : m_server(new QLocalServer(this))
    {
        connect(m_server, &QLocalServer::newConnection, this, [this] {
            while (QLocalSocket *client = m_server->nextPendingConnection()) {
                connect(client, &QLocalSocket::readyRead, this, [this, client] { onReadyRead(client); });
                writeFrame(client, {{QStringLiteral("event"), QStringLiteral("shown")},
                                    {QStringLiteral("region"), QStringLiteral("main")},
                                    {QStringLiteral("path"), QStringLiteral("/x.jpg")}});
            }
        });
    }

    Q_INVOKABLE bool start(const QString &name)
    {
        QLocalServer::removeServer(name);
        return m_server->listen(name);
    }

private:
    void onReadyRead(QLocalSocket *client)
    {
        for (const QCborMap &request : readFrames(client)) {
            if (!answer)
                continue;
            QCborMap response;
            if (request.value(QStringLiteral("command")).toString() == u"status") {
                response = {{QStringLiteral("ok"), true},
                            {QStringLiteral("paused"), false},
                            {QStringLiteral("layout"), QStringLiteral("vollbild")}};
            } else {
                response = {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unbekannter Befehl")}};
            }
            response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
            writeFrame(client, response);
        }
    }

    QLocalServer *m_server;
};

class TestAdmin : public QObject
{
    Q_OBJECT

private slots:
    void credentials();
    void credentialsRejectTampering();
    void credentialsFile();
    void sessions();
    void sessionExpiry();
    void loginThrottle();
    void sanitizeFileName_data();
    void sanitizeFileName();
    void sanitizeFileNameLength();
    void albumDirectory_data();
    void albumDirectory();
    void uniqueFilePath();
    void moveToTrash();
    void updateSidecar();
    void framesRoundtrip();
    void partialFrame();
    void socketName();
    void adminSettings();
    void displayLinkOffline();
    void displayLink();
};

void TestAdmin::credentials()
{
    const Credentials credentials = Credentials::create(QStringLiteral("admin"), QStringLiteral("geheim123"), 1000);
    QVERIFY(credentials.isValid());
    QVERIFY(credentials.verify(QStringLiteral("admin"), QStringLiteral("geheim123")));
    QVERIFY(!credentials.verify(QStringLiteral("admin"), QStringLiteral("geheim124")));
    QVERIFY(!credentials.verify(QStringLiteral("Admin"), QStringLiteral("geheim123")));
    QVERIFY(!credentials.verify(QString(), QStringLiteral("geheim123")));

    // Same password, new salt: different hash
    const Credentials other = Credentials::create(QStringLiteral("admin"), QStringLiteral("geheim123"), 1000);
    QVERIFY(other.salt != credentials.salt);
    QVERIFY(other.hash != credentials.hash);

    const Credentials restored = Credentials::fromJson(credentials.toJson());
    QCOMPARE(restored.username, credentials.username);
    QCOMPARE(restored.algorithm, credentials.algorithm);
    QCOMPARE(restored.iterations, 1000);
    QCOMPARE(restored.salt, credentials.salt);
    QCOMPARE(restored.hash, credentials.hash);
    QVERIFY(restored.verify(QStringLiteral("admin"), QStringLiteral("geheim123")));
}

void TestAdmin::credentialsRejectTampering()
{
    const Credentials good = Credentials::create(QStringLiteral("admin"), QStringLiteral("geheim123"), 1000);

    Credentials flipped = good;
    flipped.hash[0] = char(flipped.hash.at(0) ^ 1);
    QVERIFY(flipped.isValid());
    QVERIFY(!flipped.verify(QStringLiteral("admin"), QStringLiteral("geheim123")));

    Credentials shortSalt = good;
    shortSalt.salt.truncate(8);
    QVERIFY(!shortSalt.isValid());
    QVERIFY(!shortSalt.verify(QStringLiteral("admin"), QStringLiteral("geheim123")));

    Credentials shortHash = good;
    shortHash.hash.chop(1);
    QVERIFY(!shortHash.isValid());

    Credentials algorithm = good;
    algorithm.algorithm = QStringLiteral("md5");
    QVERIFY(!algorithm.isValid());

    Credentials fewIterations = good;
    fewIterations.iterations = 10;
    QVERIFY(!fewIterations.isValid());

    QVERIFY(!Credentials::fromJson({}).isValid());
    QVERIFY(!Credentials().verify(QString(), QString()));
}

void TestAdmin::credentialsFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = credentialsPath(dir.filePath(QStringLiteral("state")));
    QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("auth.json"));

    // Missing file: no account
    QVERIFY(!loadCredentials(path).isValid());

    const Credentials credentials = Credentials::create(QStringLiteral("leo"), QStringLiteral("naturfotos"), 1000);
    QString error;
    QVERIFY2(saveCredentials(path, credentials, &error), qPrintable(error));

    const QFileDevice::Permissions permissions = QFileInfo(path).permissions();
    QVERIFY(permissions & QFileDevice::ReadOwner);
    QVERIFY(permissions & QFileDevice::WriteOwner);
    const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    QCOMPARE(permissions & others, QFileDevice::Permissions());

    const Credentials loaded = loadCredentials(path);
    QVERIFY(loaded.isValid());
    QVERIFY(loaded.verify(QStringLiteral("leo"), QStringLiteral("naturfotos")));
    QCOMPARE(loaded.stamp(), credentials.stamp());
}

void TestAdmin::sessions()
{
    SessionStore store(60000);
    const QString token = store.create(QStringLiteral("admin"), "stempel");
    QVERIFY(token.size() >= 40);
    QVERIFY(store.create(QStringLiteral("admin"), "stempel") != token);

    QCOMPARE(store.validate(token, "stempel"), std::optional<QString>(QStringLiteral("admin")));
    QVERIFY(!store.validate(QStringLiteral("erfunden"), "stempel"));

    // A new password (other stamp) ends the session for good
    QVERIFY(!store.validate(token, "neuer-stempel"));
    QVERIFY(!store.validate(token, "stempel"));

    const QString second = store.create(QStringLiteral("admin"), "stempel");
    QVERIFY(store.validate(second, "stempel"));
    store.revoke(second);
    QVERIFY(!store.validate(second, "stempel"));
}

void TestAdmin::sessionExpiry()
{
    SessionStore shortLived(50);
    const QString token = shortLived.create(QStringLiteral("admin"), "s");
    QTest::qWait(120);
    QVERIFY(!shortLived.validate(token, "s"));

    // Every valid use extends the session
    SessionStore sliding(400);
    const QString used = sliding.create(QStringLiteral("admin"), "s");
    const QString idle = sliding.create(QStringLiteral("admin"), "s");
    QTest::qWait(250);
    QVERIFY(sliding.validate(used, "s"));
    QTest::qWait(250);
    QVERIFY(sliding.validate(used, "s"));
    QVERIFY(!sliding.validate(idle, "s"));
}

void TestAdmin::loginThrottle()
{
    LoginThrottle throttle(3, 100, 1000);
    const QHostAddress client(QStringLiteral("192.168.1.5"));
    const QHostAddress neighbour(QStringLiteral("192.168.1.6"));

    QVERIFY(throttle.allowed(client));
    throttle.recordFailure(client);
    throttle.recordFailure(client);
    QVERIFY(throttle.allowed(client));

    throttle.recordFailure(client);
    qint64 retryAfter = 0;
    QVERIFY(!throttle.allowed(client, &retryAfter));
    QVERIFY(retryAfter > 0 && retryAfter <= 100);
    QVERIFY(throttle.allowed(neighbour));

    // The same client over IPv6 (IPv4-mapped) is still blocked
    QVERIFY(!throttle.allowed(QHostAddress(QStringLiteral("::ffff:192.168.1.5"))));

    QTest::qWait(130);
    QVERIFY(throttle.allowed(client));

    // The next failure doubles the wait
    throttle.recordFailure(QHostAddress(QStringLiteral("::ffff:192.168.1.5")));
    QVERIFY(!throttle.allowed(client, &retryAfter));
    QVERIFY(retryAfter > 100 && retryAfter <= 200);

    throttle.recordSuccess(client);
    QVERIFY(throttle.allowed(client));

    // The wait never exceeds the maximum
    LoginThrottle capped(1, 100, 150);
    for (int i = 0; i < 10; ++i)
        capped.recordFailure(client);
    QVERIFY(!capped.allowed(client, &retryAfter));
    QVERIFY(retryAfter > 0 && retryAfter <= 150);
}

void TestAdmin::sanitizeFileName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("format");
    QTest::addColumn<QString>("expected");

    QTest::newRow("plain") << QStringLiteral("elch.jpg") << QByteArray("jpeg") << QStringLiteral("elch.jpg");
    QTest::newRow("windows fakepath") << QStringLiteral("C:\\fakepath\\Elch im Nebel.JPG") << QByteArray("jpeg")
                                      << QStringLiteral("Elch im Nebel.jpg");
    QTest::newRow("traversal") << QStringLiteral("../../x.jpg") << QByteArray("jpeg") << QStringLiteral("x.jpg");
    QTest::newRow("reserved") << QStringLiteral("a:b*c?\"<>|.png") << QByteArray("png") << QStringLiteral("abc.png");
    QTest::newRow("control") << QStringLiteral("a\tb\nc.png") << QByteArray("png") << QStringLiteral("abc.png");
    QTest::newRow("leading dots") << QStringLiteral("..versteckt.jpg") << QByteArray("jpeg") << QStringLiteral("versteckt.jpg");
    QTest::newRow("leading at") << QStringLiteral("@eaDir.jpg") << QByteArray("jpeg") << QStringLiteral("eaDir.jpg");
    QTest::newRow("wrong extension") << QStringLiteral("foto.png") << QByteArray("jpeg") << QStringLiteral("foto.png.jpg");
    QTest::newRow("no extension") << QStringLiteral("foto") << QByteArray("webp") << QStringLiteral("foto.webp");
    QTest::newRow("jpeg extension") << QStringLiteral("FOTO.JPEG") << QByteArray("jpg") << QStringLiteral("FOTO.jpeg");
    QTest::newRow("umlauts") << QStringLiteral("Größe am Fluss.jpeg") << QByteArray("jpeg")
                             << QStringLiteral("Größe am Fluss.jpeg");
    QTest::newRow("spaces") << QStringLiteral(" name .jpg ") << QByteArray("jpeg") << QStringLiteral("name.jpg");
    QTest::newRow("empty") << QString() << QByteArray("jpeg") << QStringLiteral("bild.jpg");
    QTest::newRow("only dots") << QStringLiteral("...") << QByteArray("png") << QStringLiteral("bild.png");
    QTest::newRow("unsupported") << QStringLiteral("x.gif") << QByteArray("gif") << QString();
    QTest::newRow("no format") << QStringLiteral("x.jpg") << QByteArray() << QString();
}

void TestAdmin::sanitizeFileName()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, format);
    QFETCH(QString, expected);
    QCOMPARE(berry::sanitizeFileName(name, format), expected);
}

void TestAdmin::sanitizeFileNameLength()
{
    const QString name = berry::sanitizeFileName(QString(200, u'a') + QStringLiteral(".jpg"), "jpeg");
    QCOMPARE(name.size(), 120);
    QVERIFY(name.endsWith(QStringLiteral(".jpg")));
}

void TestAdmin::albumDirectory_data()
{
    QTest::addColumn<QString>("album");
    QTest::addColumn<QString>("expected");

    const QString root = QStringLiteral("/srv/media");
    QTest::newRow("root") << QString() << root;
    QTest::newRow("slash") << QStringLiteral("/") << root;
    QTest::newRow("album") << QStringLiteral("alpen") << root + QStringLiteral("/alpen");
    QTest::newRow("nested") << QStringLiteral("reisen/norwegen") << root + QStringLiteral("/reisen/norwegen");
    QTest::newRow("backslashes") << QStringLiteral("reisen\\norwegen") << root + QStringLiteral("/reisen/norwegen");
    QTest::newRow("outer slashes") << QStringLiteral("/alpen/") << root + QStringLiteral("/alpen");
    QTest::newRow("spaces") << QStringLiteral("  alpen  ") << root + QStringLiteral("/alpen");
    QTest::newRow("umlauts") << QStringLiteral("Bäume") << root + QStringLiteral("/Bäume");
    QTest::newRow("parent") << QStringLiteral("..") << QString();
    QTest::newRow("escape") << QStringLiteral("alpen/../..") << QString();
    QTest::newRow("dot") << QStringLiteral("alpen/./x") << QString();
    QTest::newRow("hidden") << QStringLiteral(".x") << QString();
    QTest::newRow("nested hidden") << QStringLiteral("alpen/.cache") << QString();
    QTest::newRow("synology") << QStringLiteral("@eaDir") << QString();
    QTest::newRow("reserved") << QStringLiteral("a:b") << QString();
    QTest::newRow("too long") << QString(101, u'x') << QString();
}

void TestAdmin::albumDirectory()
{
    QFETCH(QString, album);
    QFETCH(QString, expected);
    QCOMPARE(berry::albumDirectory(QStringLiteral("/srv/media"), album), expected);
}

void TestAdmin::uniqueFilePath()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString first = berry::uniqueFilePath(dir.path(), QStringLiteral("x.jpg"));
    QCOMPARE(first, dir.filePath(QStringLiteral("x.jpg")));
    writeFile(first, "1");

    const QString second = berry::uniqueFilePath(dir.path(), QStringLiteral("x.jpg"));
    QCOMPARE(second, dir.filePath(QStringLiteral("x (2).jpg")));
    writeFile(second, "2");

    QCOMPARE(berry::uniqueFilePath(dir.path(), QStringLiteral("x.jpg")), dir.filePath(QStringLiteral("x (3).jpg")));
}

void TestAdmin::moveToTrash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString trash = dir.filePath(QStringLiteral("trash"));
    const QString image = media + QStringLiteral("/alpen/elch.jpg");
    writeFile(image, "bild");
    writeFile(media + QStringLiteral("/alpen/elch.json"), R"({ "title": "Elch" })");

    QString error;
    QVERIFY2(berry::moveToTrash(media, trash, image, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(image));
    QVERIFY(!QFileInfo::exists(media + QStringLiteral("/alpen/elch.json")));

    // trash/<stamp>/alpen/elch.jpg and its sidecar
    const QStringList stamps = QDir(trash).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(stamps.size(), 1);
    QVERIFY(QRegularExpression(QStringLiteral("^\\d{8}-\\d{6}$")).match(stamps.constFirst()).hasMatch());
    const QDir stamp(QDir(trash).filePath(stamps.constFirst()));
    QVERIFY(QFileInfo::exists(stamp.filePath(QStringLiteral("alpen/elch.jpg"))));
    QCOMPARE(readJson(stamp.filePath(QStringLiteral("alpen/elch.json"))).value(u"title").toString(), QStringLiteral("Elch"));

    // Pictures outside the media folder are left alone
    const QString outside = dir.filePath(QStringLiteral("anderswo/x.jpg"));
    writeFile(outside, "bild");
    QVERIFY(!berry::moveToTrash(media, trash, outside, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(QFileInfo::exists(outside));
}

void TestAdmin::updateSidecar()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString image = dir.filePath(QStringLiteral("elch.jpg"));
    const QString sidecar = dir.filePath(QStringLiteral("elch.json"));

    QString error;
    QVERIFY2(berry::updateSidecar(image, {{QStringLiteral("title"), QStringLiteral("Elch")}}, &error), qPrintable(error));
    QCOMPARE(readJson(sidecar), (QJsonObject{{QStringLiteral("title"), QStringLiteral("Elch")}}));

    QVERIFY(berry::updateSidecar(image, {{QStringLiteral("quote"), QStringLiteral("Geduld")}, {QStringLiteral("enabled"), false}}));
    QCOMPARE(readJson(sidecar).value(u"title").toString(), QStringLiteral("Elch"));
    QCOMPARE(readJson(sidecar).value(u"quote").toString(), QStringLiteral("Geduld"));

    ImageMetadata metadata;
    QVERIFY(metadata.enabled);
    applySidecar(metadata, readJson(sidecar));
    QVERIFY(!metadata.enabled);
    QCOMPARE(metadata.title, QStringLiteral("Elch"));

    // Keys written by hand survive, null removes a key
    QJsonObject withCustom = readJson(sidecar);
    withCustom.insert(QStringLiteral("eigenes"), 1);
    writeFile(sidecar, QJsonDocument(withCustom).toJson());
    QVERIFY(berry::updateSidecar(image, {{QStringLiteral("title"), QJsonValue::Null}}));
    QJsonObject current = readJson(sidecar);
    QVERIFY(!current.contains(u"title"));
    QCOMPARE(current.value(u"eigenes").toInt(), 1);
    QCOMPARE(current.value(u"quote").toString(), QStringLiteral("Geduld"));

    // No keys left: no file
    QVERIFY(berry::updateSidecar(image, {{QStringLiteral("quote"), QJsonValue::Null},
                                         {QStringLiteral("enabled"), QJsonValue::Null},
                                         {QStringLiteral("eigenes"), QJsonValue::Null}}));
    QVERIFY(!QFileInfo::exists(sidecar));

    // A broken file is never overwritten
    writeFile(sidecar, "{ kaputt");
    QVERIFY(!berry::updateSidecar(image, {{QStringLiteral("title"), QStringLiteral("x")}}, &error));
    QVERIFY(!error.isEmpty());
    QFile file(sidecar);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("{ kaputt"));
}

void TestAdmin::framesRoundtrip()
{
    QByteArray binary("\x00\x01\xfe\xff jpeg", 9);
    const QCborMap first{{QStringLiteral("id"), 1}, {QStringLiteral("command"), QStringLiteral("status")}};
    const QCborMap second{{QStringLiteral("id"), 2}, {QStringLiteral("ok"), true}, {QStringLiteral("image"), binary}};

    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    writeFrame(&buffer, first);
    writeFrame(&buffer, second);
    buffer.seek(0);

    const QList<QCborMap> frames = readFrames(&buffer);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames.at(0), first);
    QCOMPARE(frames.at(1), second);
    QCOMPARE(frames.at(1).value(QStringLiteral("image")).toByteArray(), binary);
    QVERIFY(readFrames(&buffer).isEmpty());
}

void TestAdmin::partialFrame()
{
    const QCborMap message{{QStringLiteral("id"), 7}, {QStringLiteral("command"), QStringLiteral("next")}};
    QBuffer encoder;
    QVERIFY(encoder.open(QIODevice::WriteOnly));
    writeFrame(&encoder, message);
    const QByteArray bytes = encoder.data();
    QVERIFY(bytes.size() > 6);

    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    buffer.write(bytes.left(bytes.size() - 3));
    buffer.seek(0);
    QVERIFY(readFrames(&buffer).isEmpty());
    QCOMPARE(buffer.pos(), qint64(0)); // nothing consumed

    buffer.seek(buffer.size());
    buffer.write(bytes.right(3));
    buffer.seek(0);
    const QList<QCborMap> frames = readFrames(&buffer);
    QCOMPARE(frames.size(), 1);
    QCOMPARE(frames.constFirst(), message);
}

void TestAdmin::socketName()
{
    QCOMPARE(displaySocketName(QStringLiteral("/var/lib/berry-kiosk/state")),
             QStringLiteral("/var/lib/berry-kiosk/state/display.sock"));

    const QString longDir = QStringLiteral("/") + QString(150, u'x');
    const QString name = displaySocketName(longDir);
    QVERIFY(name.startsWith(QStringLiteral("berry-kiosk-")));
    QVERIFY(!name.contains(u'/'));
    QCOMPARE(displaySocketName(longDir), name);
    QVERIFY(displaySocketName(longDir + u'y') != name);
}

void TestAdmin::adminSettings()
{
    const Config defaults = Config::fromJson({}, QStringLiteral("/srv/kiosk"));
    QCOMPARE(defaults.admin().address, QStringLiteral("0.0.0.0"));
    QCOMPARE(defaults.admin().port, quint16(8080));
    QCOMPARE(defaults.admin().maxUploadBytes, qint64(64) * 1024 * 1024);
    QCOMPARE(defaults.library().trashDir, QStringLiteral("/srv/kiosk/trash"));

    auto admin = [](const QJsonObject &settings) {
        return Config::fromJson(QJsonObject{{QStringLiteral("admin"), settings}}, QStringLiteral("/")).admin();
    };
    const AdminSettings custom = admin({{QStringLiteral("address"), QStringLiteral("127.0.0.1")},
                                        {QStringLiteral("port"), 9000},
                                        {QStringLiteral("maxUploadMB"), 10}});
    QCOMPARE(custom.address, QStringLiteral("127.0.0.1"));
    QCOMPARE(custom.port, quint16(9000));
    QCOMPARE(custom.maxUploadBytes, qint64(10) * 1024 * 1024);

    QCOMPARE(admin({{QStringLiteral("address"), QStringLiteral("  ")}}).address, QStringLiteral("0.0.0.0"));
    QCOMPARE(admin({{QStringLiteral("port"), 0}}).port, quint16(8080));
    QCOMPARE(admin({{QStringLiteral("port"), 70000}}).port, quint16(8080));
    QCOMPARE(admin({{QStringLiteral("maxUploadMB"), 5000}}).maxUploadBytes, qint64(1024) * 1024 * 1024);
    QCOMPARE(admin({{QStringLiteral("maxUploadMB"), 0}}).maxUploadBytes, qint64(1) * 1024 * 1024);
}

void TestAdmin::displayLinkOffline()
{
    DisplayLink link;
    QVERIFY(!link.isConnected());
    QString error;
    QVERIFY(link.request(QStringLiteral("status"), {}, 100, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestAdmin::displayLink()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString name = displaySocketName(dir.path());

    QThread thread;
    auto *fake = new FakeDisplay;
    fake->moveToThread(&thread);
    connect(&thread, &QThread::finished, fake, &QObject::deleteLater);
    thread.start();
    const auto stop = qScopeGuard([&thread] {
        thread.quit();
        thread.wait();
    });

    bool listening = false;
    QVERIFY(QMetaObject::invokeMethod(fake, "start", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, listening),
                                      Q_ARG(QString, name)));
    QVERIFY(listening);

    DisplayLink link;
    QSignalSpy connected(&link, &DisplayLink::connectedChanged);
    link.setSocketName(name);
    QTRY_VERIFY(link.isConnected());
    QVERIFY(!connected.isEmpty());
    QCOMPARE(connected.first().first().toBool(), true);

    // The display reports what it shows on its own
    QTRY_COMPARE(link.shown().value(QStringLiteral("main")), QStringLiteral("/x.jpg"));

    QString error;
    const QCborMap status = link.request(QStringLiteral("status"), {}, 2000, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(status.value(QStringLiteral("ok")).toBool());
    QCOMPARE(status.value(QStringLiteral("layout")).toString(), QStringLiteral("vollbild"));
    QCOMPARE(status.value(QStringLiteral("paused")).toBool(), false);

    QVERIFY(link.request(QStringLiteral("tanzen"), {}, 2000, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("unbekannter Befehl"));

    // No answer: gives up after the timeout
    fake->answer = false;
    error.clear();
    QElapsedTimer timer;
    timer.start();
    QVERIFY(link.request(QStringLiteral("status"), {}, 200, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(timer.elapsed() >= 150);

    // Answers flow again afterwards
    fake->answer = true;
    error.clear();
    QVERIFY(!link.request(QStringLiteral("status"), {}, 2000, &error).isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

QTEST_GUILESS_MAIN(TestAdmin)
#include "tst_admin.moc"
