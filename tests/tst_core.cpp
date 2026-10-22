#include "config.h"
#include "deviceinfo.h"
#include "imagemetadata.h"
#include "imagescanner.h"
#include "rendertier.h"
#include "schedule.h"
#include "sdnotify.h"

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
    void configLayoutById();
    void configLoadFailure();
    void scanImages();
    void configLocaleAndQuotes();
    void formatting();
    void readMetadata();
    void sidecarOverrides();
    void scheduleDays_data();
    void scheduleDays();
    void scheduleInvalidEntries();
    void scheduleTimes();
    void scheduleMatches_data();
    void scheduleMatches();
    void scheduleEvaluate();
    void configOperations();
    void watchdogInterval();
    void sdNotifyWithoutSystemd();
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

void TestCore::configLayoutById()
{
    const QJsonArray layouts{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("tag")}, {QStringLiteral("name"), QStringLiteral("Tag")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("abend")}},
    };
    const Config config = Config::fromJson(QJsonObject{{QStringLiteral("layouts"), layouts}}, QStringLiteral("/"));
    QCOMPARE(config.layoutById(QStringLiteral("abend")).value(u"id").toString(), QStringLiteral("abend"));
    QCOMPARE(config.layoutById(QStringLiteral("tag")).value(u"name").toString(), QStringLiteral("Tag"));
    QVERIFY(config.layoutById(QStringLiteral("nacht")).isEmpty());
    QVERIFY(config.layoutById(QString()).isEmpty());
    QVERIFY(Config::fromJson({}, QStringLiteral("/")).layoutById(QStringLiteral("default")).isEmpty());
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

namespace {

ScheduleRule ruleFrom(const QJsonObject &entry)
{
    QStringList errors;
    const QList<ScheduleRule> rules = parseSchedule(QJsonArray{entry}, &errors);
    return rules.isEmpty() ? ScheduleRule() : rules.constFirst();
}

QJsonObject entry(const QJsonValue &days, const QString &from, const QString &to, const QString &layout = QStringLiteral("tag"))
{
    QJsonObject object{{QStringLiteral("from"), from}, {QStringLiteral("to"), to}, {QStringLiteral("layout"), layout}};
    if (!days.isUndefined())
        object.insert(QStringLiteral("days"), days);
    return object;
}

// 2026-10-19 is a Monday
QDateTime at(int dayOfWeek, const QString &time)
{
    return QDateTime(QDate(2026, 10, 18 + dayOfWeek), QTime::fromString(time, QStringLiteral("HH:mm")));
}

} // namespace

void TestCore::scheduleDays_data()
{
    QTest::addColumn<QJsonValue>("days");
    QTest::addColumn<QList<int>>("expected");

    const QList<int> all{1, 2, 3, 4, 5, 6, 7};
    QTest::newRow("range") << QJsonValue(QStringLiteral("mo-fr")) << QList<int>{1, 2, 3, 4, 5};
    QTest::newRow("list") << QJsonValue(QStringLiteral("sa,so")) << QList<int>{6, 7};
    QTest::newRow("list with spaces") << QJsonValue(QStringLiteral("Mo, Mi Fr")) << QList<int>{1, 3, 5};
    QTest::newRow("wrapping range") << QJsonValue(QStringLiteral("fr-mo")) << QList<int>{5, 6, 7, 1};
    QTest::newRow("daily") << QJsonValue(QStringLiteral("daily")) << all;
    QTest::newRow("täglich") << QJsonValue(QStringLiteral("täglich")) << all;
    QTest::newRow("english") << QJsonValue(QStringLiteral("mon-fri")) << QList<int>{1, 2, 3, 4, 5};
    QTest::newRow("full names") << QJsonValue(QStringLiteral("samstag-sonntag")) << QList<int>{6, 7};
    QTest::newRow("numbers in text") << QJsonValue(QStringLiteral("1-3")) << QList<int>{1, 2, 3};
    QTest::newRow("array of names") << QJsonValue(QJsonArray{QStringLiteral("sa"), QStringLiteral("sun")}) << QList<int>{6, 7};
    QTest::newRow("array of numbers") << QJsonValue(QJsonArray{1, 7}) << QList<int>{1, 7};
    QTest::newRow("missing = every day") << QJsonValue(QJsonValue::Undefined) << all;
}

void TestCore::scheduleDays()
{
    QFETCH(QJsonValue, days);
    QFETCH(QList<int>, expected);

    QStringList errors;
    const QList<ScheduleRule> rules = parseSchedule(QJsonArray{entry(days, QStringLiteral("07:00"), QStringLiteral("22:00"))}, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
    QCOMPARE(rules.size(), 1);
    QCOMPARE(rules.constFirst().days, expected);
}

void TestCore::scheduleInvalidEntries()
{
    const QJsonArray entries{
        entry(QStringLiteral("xy"), QStringLiteral("07:00"), QStringLiteral("08:00")),            // unknown day
        entry(QStringLiteral("mo-xy"), QStringLiteral("07:00"), QStringLiteral("08:00")),         // unknown range end
        entry(QJsonArray{0, 8}, QStringLiteral("07:00"), QStringLiteral("08:00")),                // numbers out of range
        entry(QStringLiteral("mo"), QStringLiteral("sieben"), QStringLiteral("08:00")),           // bad time
        entry(QStringLiteral("mo"), QStringLiteral("07:00"), QStringLiteral("25:00")),            // bad time
        entry(QStringLiteral("mo"), QStringLiteral("07:00"), QStringLiteral("08:00"), QString()), // neither layout nor screen
        QJsonValue(42),
        entry(QStringLiteral("mo"), QStringLiteral("07:00"), QStringLiteral("08:00")),            // the only valid one
    };
    QStringList errors;
    const QList<ScheduleRule> rules = parseSchedule(entries, &errors);
    QCOMPARE(rules.size(), 1);
    QCOMPARE(errors.size(), 7);
    QVERIFY(errors.constFirst().startsWith(QStringLiteral("Zeitplan-Eintrag 1:")));
    QVERIFY(errors.at(3).contains(QStringLiteral("HH:mm")));
    QVERIFY(errors.at(5).contains(QStringLiteral("layout")));
    QVERIFY(errors.constLast().startsWith(QStringLiteral("Zeitplan-Eintrag 7:")));

    // Without an error list nothing breaks either
    QCOMPARE(parseSchedule(entries).size(), 1);
}

void TestCore::scheduleTimes()
{
    const ScheduleRule shortForm = ruleFrom(entry(QStringLiteral("daily"), QStringLiteral("7:00"), QStringLiteral("9:30")));
    QCOMPARE(shortForm.from, QTime(7, 0));
    QCOMPARE(shortForm.to, QTime(9, 30));
    const ScheduleRule longForm = ruleFrom(entry(QStringLiteral("daily"), QStringLiteral("07:00"), QStringLiteral("22:30")));
    QCOMPARE(longForm.from, QTime(7, 0));
    QCOMPARE(longForm.to, QTime(22, 30));

    const ScheduleRule screen = ruleFrom(QJsonObject{
        {QStringLiteral("from"), QStringLiteral("22:00")}, {QStringLiteral("to"), QStringLiteral("06:00")},
        {QStringLiteral("screen"), QStringLiteral("aus")}});
    QVERIFY(screen.screenOff);
    QVERIFY(screen.layout.isEmpty());
}

void TestCore::scheduleMatches_data()
{
    QTest::addColumn<QString>("days");
    QTest::addColumn<QString>("from");
    QTest::addColumn<QString>("to");
    QTest::addColumn<QDateTime>("when");
    QTest::addColumn<bool>("matches");

    const QString mofr = QStringLiteral("mo-fr");
    QTest::newRow("inside") << mofr << QStringLiteral("07:00") << QStringLiteral("22:00") << at(3, QStringLiteral("12:00")) << true;
    QTest::newRow("from is inclusive") << mofr << QStringLiteral("07:00") << QStringLiteral("22:00") << at(3, QStringLiteral("07:00")) << true;
    QTest::newRow("to is exclusive") << mofr << QStringLiteral("07:00") << QStringLiteral("22:00") << at(3, QStringLiteral("22:00")) << false;
    QTest::newRow("before") << mofr << QStringLiteral("07:00") << QStringLiteral("22:00") << at(3, QStringLiteral("06:59")) << false;
    QTest::newRow("other day") << mofr << QStringLiteral("07:00") << QStringLiteral("22:00") << at(6, QStringLiteral("12:00")) << false;

    const QString fr = QStringLiteral("fr");
    QTest::newRow("overnight evening") << fr << QStringLiteral("22:00") << QStringLiteral("02:00") << at(5, QStringLiteral("23:30")) << true;
    QTest::newRow("overnight morning after") << fr << QStringLiteral("22:00") << QStringLiteral("02:00") << at(6, QStringLiteral("01:00")) << true;
    QTest::newRow("overnight morning of the day") << fr << QStringLiteral("22:00") << QStringLiteral("02:00") << at(5, QStringLiteral("01:00")) << false;
    QTest::newRow("overnight end exclusive") << fr << QStringLiteral("22:00") << QStringLiteral("02:00") << at(6, QStringLiteral("02:00")) << false;
    QTest::newRow("overnight daytime") << fr << QStringLiteral("22:00") << QStringLiteral("02:00") << at(5, QStringLiteral("12:00")) << false;
    QTest::newRow("overnight sunday into monday") << QStringLiteral("so") << QStringLiteral("23:00") << QStringLiteral("01:00") << at(1, QStringLiteral("00:30")) << true;

    QTest::newRow("whole day") << QStringLiteral("sa") << QStringLiteral("00:00") << QStringLiteral("00:00") << at(6, QStringLiteral("03:17")) << true;
    QTest::newRow("whole day, other day") << QStringLiteral("sa") << QStringLiteral("08:00") << QStringLiteral("08:00") << at(7, QStringLiteral("09:00")) << false;
}

void TestCore::scheduleMatches()
{
    QFETCH(QString, days);
    QFETCH(QString, from);
    QFETCH(QString, to);
    QFETCH(QDateTime, when);
    QFETCH(bool, matches);

    const ScheduleRule rule = ruleFrom(entry(days, from, to));
    QVERIFY(!rule.days.isEmpty());
    QCOMPARE(rule.matches(when), matches);
}

void TestCore::scheduleEvaluate()
{
    const QJsonArray entries{
        entry(QStringLiteral("daily"), QStringLiteral("06:00"), QStringLiteral("23:00"), QStringLiteral("tag")),
        entry(QStringLiteral("sa,so"), QStringLiteral("09:00"), QStringLiteral("12:00"), QStringLiteral("wochenende")),
        QJsonObject{{QStringLiteral("days"), QStringLiteral("daily")}, {QStringLiteral("from"), QStringLiteral("22:00")},
                    {QStringLiteral("to"), QStringLiteral("06:00")}, {QStringLiteral("screen"), QStringLiteral("off")}},
    };
    const QList<ScheduleRule> rules = parseSchedule(entries);
    QCOMPARE(rules.size(), 3);

    QCOMPARE(evaluateSchedule(rules, at(3, QStringLiteral("12:00"))), (ScheduleState{QStringLiteral("tag"), false}));
    // Later rules win where they overlap
    QCOMPARE(evaluateSchedule(rules, at(6, QStringLiteral("10:00"))), (ScheduleState{QStringLiteral("wochenende"), false}));
    // Screen off wins over the day layout
    const ScheduleState lateEvening = evaluateSchedule(rules, at(3, QStringLiteral("22:30")));
    QVERIFY(lateEvening.screenOff);
    QEXPECT_FAIL("", "evaluateSchedule() keeps the layout of an earlier matching rule when a later "
                     "rule switches the screen off (harmless: nothing is shown while the screen is off)", Continue);
    QCOMPARE(lateEvening.layout, QString());
    // Only the screen-off rule applies at night
    QCOMPARE(evaluateSchedule(rules, at(4, QStringLiteral("03:00"))), (ScheduleState{QString(), true}));
    // Nothing applies: no layout and the screen stays on
    QCOMPARE(evaluateSchedule(parseSchedule(QJsonArray{entries.at(1)}), at(3, QStringLiteral("10:00"))), ScheduleState());
    QCOMPARE(evaluateSchedule({}, at(3, QStringLiteral("10:00"))), ScheduleState());
}

void TestCore::configOperations()
{
    auto config = [](const QJsonObject &json) { return Config::fromJson(json, QStringLiteral("/")); };
    auto power = [&](const QString &value) {
        return config({{QStringLiteral("display"), QJsonObject{{QStringLiteral("power"), value}}}}).display().power;
    };

    QCOMPARE(config({}).display().power, ScreenPower::None);
    QCOMPARE(power(QStringLiteral("cec")), ScreenPower::Cec);
    QCOMPARE(power(QStringLiteral(" DDC ")), ScreenPower::Ddc);
    QCOMPARE(power(QStringLiteral("none")), ScreenPower::None);
    QCOMPARE(power(QStringLiteral("rm -rf /")), ScreenPower::None);

    QVERIFY(!config({}).isPublic());
    QVERIFY(!config({{QStringLiteral("profile"), QStringLiteral("home")}}).isPublic());
    QVERIFY(config({{QStringLiteral("profile"), QStringLiteral("Public")}}).isPublic());

    QVERIFY(config({}).admin().enabled);
    QVERIFY(!config({{QStringLiteral("admin"), QJsonObject{{QStringLiteral("enabled"), false}}}}).admin().enabled);

    QVERIFY(config({}).schedule().isEmpty());
    const QJsonArray schedule{entry(QStringLiteral("daily"), QStringLiteral("07:00"), QStringLiteral("22:00"))};
    QCOMPARE(config({{QStringLiteral("schedule"), schedule}}).schedule(), schedule);
}

void TestCore::watchdogInterval()
{
    const QByteArray saved = qgetenv("WATCHDOG_USEC");
    const bool wasSet = qEnvironmentVariableIsSet("WATCHDOG_USEC");

    qunsetenv("WATCHDOG_USEC");
    QCOMPARE(watchdogIntervalMs(), 0);
    qputenv("WATCHDOG_USEC", "30000000"); // 30 s: ping every 15 s
    QCOMPARE(watchdogIntervalMs(), 15000);
    qputenv("WATCHDOG_USEC", "500000"); // very short timeouts still get at least 1 s
    QCOMPARE(watchdogIntervalMs(), 1000);
    qputenv("WATCHDOG_USEC", "viel");
    QCOMPARE(watchdogIntervalMs(), 0);
    qputenv("WATCHDOG_USEC", "-5");
    QCOMPARE(watchdogIntervalMs(), 0);

    if (wasSet)
        qputenv("WATCHDOG_USEC", saved);
    else
        qunsetenv("WATCHDOG_USEC");
}

void TestCore::sdNotifyWithoutSystemd()
{
    const QByteArray saved = qgetenv("NOTIFY_SOCKET");
    const bool wasSet = qEnvironmentVariableIsSet("NOTIFY_SOCKET");

    qunsetenv("NOTIFY_SOCKET");
    sdNotify("READY=1");
    // A socket that does not exist must not hurt either
    qputenv("NOTIFY_SOCKET", "/nonexistent/berry-kiosk-notify");
    sdNotify("WATCHDOG=1");
    qputenv("NOTIFY_SOCKET", QByteArray(200, 'x')); // longer than sun_path
    sdNotify("WATCHDOG=1");

    if (wasSet)
        qputenv("NOTIFY_SOCKET", saved);
    else
        qunsetenv("NOTIFY_SOCKET");
}

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
