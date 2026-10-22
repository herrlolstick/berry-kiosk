#include "config.h"
#include "deviceinfo.h"
#include "imagescanner.h"
#include "rendertier.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace berry;

class TestCore : public QObject
{
    Q_OBJECT

private slots:
    void renderTier_data();
    void renderTier();
    void fitDecodeSize_data();
    void fitDecodeSize();
    void deviceInfo_data();
    void deviceInfo();
    void configDefaults();
    void configDisplay();
    void configActiveLayout();
    void configLoadFailure();
    void scanImages();
};

void TestCore::renderTier_data()
{
    QTest::addColumn<QSize>("screen");
    QTest::addColumn<int>("maxTexture");
    QTest::addColumn<bool>("limitTo1080");
    QTest::addColumn<QString>("tier");
    QTest::addColumn<QSize>("renderSize");

    const QString hd = QStringLiteral("1080p");
    const QString uhd = QStringLiteral("2160p");

    QTest::newRow("full hd") << QSize(1920, 1080) << 4096 << false << hd << QSize(1920, 1080);
    QTest::newRow("4k") << QSize(3840, 2160) << 4096 << false << uhd << QSize(3840, 2160);
    QTest::newRow("4k, gpu unknown") << QSize(3840, 2160) << 0 << false << uhd << QSize(3840, 2160);
    QTest::newRow("4k on pi 3") << QSize(3840, 2160) << 2048 << false << hd << QSize(1920, 1080);
    QTest::newRow("4k, limited") << QSize(3840, 2160) << 4096 << true << hd << QSize(1920, 1080);
    QTest::newRow("4k portrait") << QSize(2160, 3840) << 4096 << false << uhd << QSize(2160, 3840);
    QTest::newRow("hd portrait") << QSize(1080, 1920) << 4096 << false << hd << QSize(1080, 1920);
    QTest::newRow("qhd") << QSize(2560, 1440) << 4096 << false << uhd << QSize(2560, 1440);
    QTest::newRow("small panel") << QSize(1280, 800) << 2048 << false << hd << QSize(1280, 800);
    QTest::newRow("no screen") << QSize() << 4096 << false << hd << QSize(1920, 1080);
}

void TestCore::renderTier()
{
    QFETCH(QSize, screen);
    QFETCH(int, maxTexture);
    QFETCH(bool, limitTo1080);
    QFETCH(QString, tier);
    QFETCH(QSize, renderSize);

    const RenderDecision decision =
        decideRenderTier(screen, maxTexture, limitTo1080 ? MaxRender::Hd1080 : MaxRender::Uhd2160);
    QCOMPARE(tierName(decision.tier), tier);
    QCOMPARE(decision.renderSize, renderSize);
    QVERIFY(!decision.reason.isEmpty());
}

void TestCore::fitDecodeSize_data()
{
    QTest::addColumn<QSize>("original");
    QTest::addColumn<QSize>("box");
    QTest::addColumn<bool>("cover");
    QTest::addColumn<int>("maxTexture");
    QTest::addColumn<QSize>("expected");

    QTest::newRow("cover") << QSize(6000, 4000) << QSize(1920, 1080) << true << 0 << QSize(1920, 1280);
    QTest::newRow("contain") << QSize(6000, 4000) << QSize(1920, 1080) << false << 0 << QSize(1620, 1080);
    QTest::newRow("portrait cover") << QSize(4000, 6000) << QSize(1920, 1080) << true << 0 << QSize(1920, 2880);
    QTest::newRow("panorama capped") << QSize(8000, 2000) << QSize(1920, 1080) << true << 2048 << QSize(2048, 512);
    QTest::newRow("no upscaling") << QSize(800, 600) << QSize(1920, 1080) << true << 0 << QSize(800, 600);
    QTest::newRow("no box, capped") << QSize(5000, 3000) << QSize() << true << 4096 << QSize(4096, 2458);
    QTest::newRow("unreadable") << QSize() << QSize(1920, 1080) << true << 4096 << QSize();
}

void TestCore::fitDecodeSize()
{
    QFETCH(QSize, original);
    QFETCH(QSize, box);
    QFETCH(bool, cover);
    QFETCH(int, maxTexture);
    QFETCH(QSize, expected);

    QCOMPARE(berry::fitDecodeSize(original, box, cover, maxTexture), expected);
}

void TestCore::deviceInfo_data()
{
    QTest::addColumn<QString>("model");
    QTest::addColumn<bool>("isPi");
    QTest::addColumn<bool>("legacyGpu");

    QTest::newRow("pi 3b+") << QStringLiteral("Raspberry Pi 3 Model B Plus Rev 1.3") << true << true;
    QTest::newRow("zero 2 w") << QStringLiteral("Raspberry Pi Zero 2 W Rev 1.0") << true << true;
    QTest::newRow("pi 4") << QStringLiteral("Raspberry Pi 4 Model B Rev 1.5") << true << false;
    QTest::newRow("pi 400") << QStringLiteral("Raspberry Pi 400 Rev 1.0") << true << false;
    QTest::newRow("pi 5") << QStringLiteral("Raspberry Pi 5 Model B Rev 1.0") << true << false;
    QTest::newRow("pi 500") << QStringLiteral("Raspberry Pi 500 Rev 1.0") << true << false;
    QTest::newRow("cm4") << QStringLiteral("Raspberry Pi Compute Module 4 Rev 1.1") << true << false;
    QTest::newRow("other board") << QStringLiteral("Radxa ROCK 5B") << false << false;
    QTest::newRow("unknown") << QString() << false << false;
}

void TestCore::deviceInfo()
{
    QFETCH(QString, model);
    QFETCH(bool, isPi);
    QFETCH(bool, legacyGpu);

    const DeviceInfo info = DeviceInfo::fromModelString(model);
    QCOMPARE(info.isRaspberryPi, isPi);
    QCOMPARE(info.legacyGpu, legacyGpu);
}

void TestCore::configDefaults()
{
    const Config config = Config::fromJson({}, QStringLiteral("/srv/kiosk"));
    QCOMPARE(config.display().maxRender, MaxRender::Uhd2160);
    QCOMPARE(config.display().rotation, 0);
    QCOMPARE(config.mediaRoot(), QStringLiteral("/srv/kiosk/media"));

    const QJsonArray regions = config.activeLayout().value(u"regions").toArray();
    QCOMPARE(regions.size(), 1);
    QCOMPARE(regions.first().toObject().value(u"type").toString(), QStringLiteral("slideshow"));
}

void TestCore::configDisplay()
{
    auto display = [](const QJsonObject &settings) {
        return Config::fromJson(QJsonObject{{QStringLiteral("display"), settings}}, QStringLiteral("/")).display();
    };

    QCOMPARE(display({{QStringLiteral("maxRender"), QStringLiteral("1080p")}}).maxRender, MaxRender::Hd1080);
    QCOMPARE(display({{QStringLiteral("maxRender"), QStringLiteral("auto")}}).maxRender, MaxRender::Uhd2160);
    QCOMPARE(display({{QStringLiteral("rotation"), 90}}).rotation, 90);
    QCOMPARE(display({{QStringLiteral("rotation"), -90}}).rotation, 270);
    QCOMPARE(display({{QStringLiteral("rotation"), 450}}).rotation, 90);
    QCOMPARE(display({{QStringLiteral("rotation"), 45}}).rotation, 0);

    const Config absoluteMedia = Config::fromJson(
        QJsonObject{{QStringLiteral("media"), QJsonObject{{QStringLiteral("path"), QStringLiteral("/mnt/fotos/")}}}},
        QStringLiteral("/srv/kiosk"));
    QCOMPARE(absoluteMedia.mediaRoot(), QStringLiteral("/mnt/fotos"));
}

void TestCore::configActiveLayout()
{
    const QJsonArray layouts{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("tag")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("abend")}},
    };
    auto active = [&](const QString &id) {
        const QJsonObject json{{QStringLiteral("layouts"), layouts}, {QStringLiteral("activeLayout"), id}};
        return Config::fromJson(json, QStringLiteral("/")).activeLayout().value(u"id").toString();
    };

    QCOMPARE(active(QStringLiteral("abend")), QStringLiteral("abend"));
    QCOMPARE(active(QStringLiteral("gibt-es-nicht")), QStringLiteral("tag"));
    QCOMPARE(active(QString()), QStringLiteral("tag"));
}

void TestCore::configLoadFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QString error;
    Config config = Config::load(dir.filePath(QStringLiteral("fehlt.json")), &error);
    QVERIFY(!error.isEmpty());
    QCOMPARE(config.mediaRoot(), dir.filePath(QStringLiteral("media")));

    QFile broken(dir.filePath(QStringLiteral("kaputt.json")));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("{ \"display\": ");
    broken.close();
    config = Config::load(broken.fileName(), &error);
    QVERIFY(!error.isEmpty());
    QCOMPARE(config.display().rotation, 0);
}

void TestCore::scanImages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QStringList files = {
        QStringLiteral("a.jpg"),
        QStringLiteral("B.JPG"),
        QStringLiteral("sub/c.png"),
        QStringLiteral("sub/d.webp"),
        QStringLiteral(".versteckt.jpg"),
        QStringLiteral("._a.jpg"),
        QStringLiteral("notizen.txt"),
        QStringLiteral(".cache/e.jpg"),
        QStringLiteral("@eaDir/f.jpg"),
    };
    for (const QString &name : files) {
        const QString path = dir.filePath(name);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    QStringList found = berry::scanImages(dir.path());
    for (QString &path : found)
        path = QDir(dir.path()).relativeFilePath(path);

    const QStringList expected = {
        QStringLiteral("a.jpg"),
        QStringLiteral("B.JPG"),
        QStringLiteral("sub/c.png"),
        QStringLiteral("sub/d.webp"),
    };
    QCOMPARE(found, expected);
    QVERIFY(berry::scanImages(dir.filePath(QStringLiteral("gibt-es-nicht"))).isEmpty());
}

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
