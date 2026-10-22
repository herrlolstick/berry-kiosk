#include "config.h"
#include "deviceinfo.h"
#include "imagemetadata.h"
#include "imagescanner.h"
#include "rendertier.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <exiv2/exiv2.hpp>

using namespace berry;

class TestCore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
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
    void configLocaleAndQuotes();
    void formatting();
    void readMetadata();
    void sidecarOverrides();
};

void TestCore::initTestCase()
{
    initializeMetadataReading();
}

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

void TestCore::configLocaleAndQuotes()
{
    const Config defaults = Config::fromJson({}, QStringLiteral("/"));
    QCOMPARE(defaults.locale(), QStringLiteral("de_DE"));
    QVERIFY(defaults.quotes().isEmpty());

    const QJsonObject json{
        {QStringLiteral("locale"), QStringLiteral("en_GB")},
        {QStringLiteral("quotes"), QJsonArray{
             QStringLiteral("Nur Text"),
             QJsonObject{{QStringLiteral("text"), QStringLiteral("Mit Autor")}, {QStringLiteral("author"), QStringLiteral("Jemand")}},
             QJsonObject{{QStringLiteral("text"), QStringLiteral("  ")}},
             42,
         }},
    };
    const Config config = Config::fromJson(json, QStringLiteral("/"));
    QCOMPARE(config.locale(), QStringLiteral("en_GB"));
    const QJsonArray quotes = config.quotes();
    QCOMPARE(quotes.size(), 2);
    QCOMPARE(quotes.at(0).toObject().value(u"text").toString(), QStringLiteral("Nur Text"));
    QCOMPARE(quotes.at(1).toObject().value(u"author").toString(), QStringLiteral("Jemand"));
}

void TestCore::formatting()
{
    const QLocale de(QLocale::German, QLocale::Germany);
    const QLocale en(QLocale::English, QLocale::UnitedKingdom);

    QCOMPARE(formatAperture(5.6, de), QStringLiteral("f/5,6"));
    QCOMPARE(formatAperture(5.6, en), QStringLiteral("f/5.6"));
    QCOMPARE(formatAperture(8, de), QStringLiteral("f/8"));
    QCOMPARE(formatAperture(0, de), QString());

    QCOMPARE(formatExposureTime(1.0 / 250, de), QStringLiteral("1/250 s"));
    QCOMPARE(formatExposureTime(0.5, de), QStringLiteral("0,5 s"));
    QCOMPARE(formatExposureTime(30, de), QStringLiteral("30 s"));
    QCOMPARE(formatFocalLength(400, de), QStringLiteral("400 mm"));
    QCOMPARE(formatFocalLength(18.5, de), QStringLiteral("18,5 mm"));

    QCOMPARE(formatCamera(QStringLiteral("NIKON CORPORATION"), QStringLiteral("NIKON Z 6_2")), QStringLiteral("NIKON Z 6_2"));
    QCOMPARE(formatCamera(QStringLiteral("Canon"), QStringLiteral("Canon EOS R5")), QStringLiteral("Canon EOS R5"));
    QCOMPARE(formatCamera(QStringLiteral("SONY"), QStringLiteral("ILCE-7M3")), QStringLiteral("SONY ILCE-7M3"));
    QCOMPARE(formatCamera(QStringLiteral("FUJIFILM"), QString()), QStringLiteral("FUJIFILM"));

    QCOMPARE(joinLocation({QString(), QStringLiteral("Kiruna"), QStringLiteral("kiruna"), QStringLiteral("Schweden")}),
             QStringLiteral("Kiruna, Schweden"));

    ImageMetadata m;
    m.iso = 1600; // no group separator: "ISO 1600", not "ISO 1.600"
    m.exposureTime = 1.0 / 800;
    QCOMPARE(formatExposureSettings(m, de), QStringLiteral("1/800 s · ISO 1600"));
}

void TestCore::readMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("elch.jpg"));

    QImage pixels(64, 48, QImage::Format_RGB32);
    pixels.fill(Qt::darkGreen);
    QVERIFY(pixels.save(path, "JPEG"));

    {
        auto image = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
        image->readMetadata();
        Exiv2::ExifData &exif = image->exifData();
        exif["Exif.Image.Make"] = std::string("NIKON CORPORATION");
        exif["Exif.Image.Model"] = std::string("NIKON Z 6_2");
        exif["Exif.Image.ImageDescription"] = std::string("NIKON DIGITAL CAMERA");
        exif["Exif.Photo.LensModel"] = std::string("NIKKOR Z 100-400mm f/4.5-5.6 VR S");
        exif["Exif.Photo.FNumber"] = Exiv2::URational(56, 10);
        exif["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 800);
        exif["Exif.Photo.FocalLength"] = Exiv2::URational(400, 1);
        exif["Exif.Photo.ISOSpeedRatings"] = uint16_t(1600);
        exif["Exif.Photo.DateTimeOriginal"] = std::string("2024:06:12 05:31:10");
        Exiv2::XmpData &xmp = image->xmpData();
        xmp["Xmp.dc.title"] = std::string("lang=\"x-default\" Elch im Morgennebel");
        xmp["Xmp.photoshop.City"] = std::string("Kiruna");
        xmp["Xmp.photoshop.Country"] = std::string("Schweden");
        image->writeMetadata();
    }

    const ImageMetadata m = readImageMetadata(path);
    QCOMPARE(m.title, QStringLiteral("Elch im Morgennebel"));
    QCOMPARE(m.description, QString()); // camera boilerplate is ignored
    QCOMPARE(m.location, QStringLiteral("Kiruna, Schweden"));
    QCOMPARE(m.camera, QStringLiteral("NIKON Z 6_2"));
    QCOMPARE(m.lens, QStringLiteral("NIKKOR Z 100-400mm f/4.5-5.6 VR S"));
    QCOMPARE(m.taken, QDateTime(QDate(2024, 6, 12), QTime(5, 31, 10)));
    QCOMPARE(formatExposureSettings(m, QLocale(QLocale::German)), QStringLiteral("f/5,6 · 1/800 s · ISO 1600 · 400 mm"));

    // Files without metadata (or no image at all) must not throw
    QFile text(dir.filePath(QStringLiteral("kein-bild.jpg")));
    QVERIFY(text.open(QIODevice::WriteOnly));
    text.write("kein JPEG");
    text.close();
    QCOMPARE(readImageMetadata(text.fileName()).title, QString());
}

void TestCore::sidecarOverrides()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("fuchs.jpeg"));
    QImage pixels(32, 32, QImage::Format_RGB32);
    pixels.fill(Qt::darkYellow);
    QVERIFY(pixels.save(path, "JPEG"));

    QCOMPARE(sidecarPath(path), dir.filePath(QStringLiteral("fuchs.json")));
    QFile sidecar(sidecarPath(path));
    QVERIFY(sidecar.open(QIODevice::WriteOnly));
    sidecar.write(R"({ "title": "Rotfuchs", "location": "Eifel", "quote": "Geduld ist alles.", "quoteAuthor": "Unbekannt" })");
    sidecar.close();

    const ImageMetadata m = readImageMetadata(path);
    QCOMPARE(m.title, QStringLiteral("Rotfuchs"));
    QCOMPARE(m.location, QStringLiteral("Eifel"));
    QCOMPARE(m.quote, QStringLiteral("Geduld ist alles."));
    QCOMPARE(m.quoteAuthor, QStringLiteral("Unbekannt"));
}

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
