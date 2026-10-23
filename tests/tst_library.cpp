#include "config.h"
#include "imagemetadata.h"
#include "imagescanner.h"
#include "library.h"
#include "mediafiles.h"
#include "renditions.h"
#include "state.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <exiv2/exiv2.hpp>

using namespace berry;

namespace {

void writeImage(const QString &path, QSize size, QColor color = Qt::darkGreen)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    QVERIFY(image.save(path, "JPEG"));
}

// Library::sync() compares modification times; make sure a rewrite counts as a change
void touchLater(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
}

// Library ids by path relative to the media root
QHash<QString, qint64> idsByPath(const QString &database, const QString &media, const QString &cache)
{
    QList<LibraryEntry> entries;
    QHash<QString, qint64> ids;
    if (!readLibrary(database, media, cache, QString(), &entries))
        return ids;
    for (const LibraryEntry &entry : std::as_const(entries))
        ids.insert(QDir(media).relativeFilePath(entry.original), entry.id);
    return ids;
}

void renderAll(Library &library, const QStringList &kinds)
{
    for (const RenditionJob &job : library.pendingRenditions(kinds, 1000))
        QVERIFY(library.generate(job, 80));
    QCOMPARE(library.pendingCount(kinds), 0);
}

QStringList relativeOriginals(const QList<LibraryEntry> &entries, const QString &root)
{
    QStringList paths;
    for (const LibraryEntry &entry : entries)
        paths.append(QDir(root).relativeFilePath(entry.original));
    return paths;
}

} // namespace

class TestLibrary : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void renditionSize_data();
    void renditionSize();
    void renditionIsUpright();
    void renditionFlattensAlpha();
    void configLibrarySettings();
    void fileSnapshot();
    void syncAndRenditions();
    void brokenFilesAreNotRetried();
    void missingMediaRootKeepsLibrary();
    void syncDetectsMoves();
    void movedAlbumKeepsRenditions();
    void renamedOrChangedFilesAreNotMoves();
    void renditionFilePathOnlyKnownKinds();
    void scanIgnoresSymlinks();
    void albumDirectoryRejectsSymlinkEscape();
    void failedRenditionIsRecordedAndRetriedAfterSuccess();
};

void TestLibrary::initTestCase()
{
    initializeMetadataReading();
    QVERIFY(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")));
}

void TestLibrary::renditionSize_data()
{
    QTest::addColumn<QSize>("original");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QSize>("expected");

    const QString hd = QStringLiteral("1080p");
    const QString uhd = QStringLiteral("2160p");
    const QString thumb = QStringLiteral("thumb");

    QTest::newRow("3:2 at 1080p") << QSize(6000, 4000) << hd << QSize(2048, 1365);
    QTest::newRow("3:2 at 2160p") << QSize(6000, 4000) << uhd << QSize(4096, 2731);
    QTest::newRow("portrait at 2160p") << QSize(4000, 6000) << uhd << QSize(2731, 4096);
    QTest::newRow("16:9 at 1080p") << QSize(3840, 2160) << hd << QSize(2048, 1152);
    QTest::newRow("panorama at 1080p") << QSize(8000, 2000) << hd << QSize(2048, 512);
    QTest::newRow("small stays small") << QSize(800, 600) << uhd << QSize(800, 600);
    QTest::newRow("thumbnail") << QSize(6000, 4000) << thumb << QSize(512, 341);
}

void TestLibrary::renditionSize()
{
    QFETCH(QSize, original);
    QFETCH(QString, kind);
    QFETCH(QSize, expected);
    QCOMPARE(berry::renditionSize(original, kind), expected);
}

void TestLibrary::renditionIsUpright()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("hochkant.jpg"));

    // Stored landscape, left half red, right half blue; EXIF says "rotate 90° clockwise"
    QImage stored(300, 200, QImage::Format_RGB32);
    stored.fill(Qt::blue);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 150; ++x)
            stored.setPixel(x, y, qRgb(255, 0, 0));
    QVERIFY(stored.save(source, "JPEG", 95));
    {
        auto image = Exiv2::ImageFactory::open(QFile::encodeName(source).toStdString());
        image->readMetadata();
        image->exifData()["Exif.Image.Orientation"] = uint16_t(6);
        image->writeMetadata();
    }
    QCOMPARE(uprightImageSize(source), QSize(200, 300));

    const QString target = dir.filePath(QStringLiteral("cache/thumb/1.jpg"));
    QSize size;
    QString error;
    QVERIFY2(createRendition(source, target, QStringLiteral("thumb"), 90, &size, &error), qPrintable(error));
    QCOMPARE(size, QSize(200, 300));

    // Renditions carry no EXIF orientation, the pixels themselves are upright:
    // the stored left half is now on top
    const QImage result(target);
    QCOMPARE(result.size(), QSize(200, 300));
    QVERIFY(qRed(result.pixel(100, 50)) > 200 && qBlue(result.pixel(100, 50)) < 60);
    QVERIFY(qBlue(result.pixel(100, 250)) > 200 && qRed(result.pixel(100, 250)) < 60);
}

void TestLibrary::renditionFlattensAlpha()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("logo.png"));
    QImage transparent(64, 64, QImage::Format_ARGB32);
    transparent.fill(Qt::transparent);
    QVERIFY(transparent.save(source, "PNG"));

    const QString target = dir.filePath(QStringLiteral("out.jpg"));
    QVERIFY(createRendition(source, target, QStringLiteral("1080p"), 90));
    const QImage result(target);
    QVERIFY(qGray(result.pixel(32, 32)) < 10);
}

void TestLibrary::configLibrarySettings()
{
    const LibrarySettings defaults = Config::fromJson({}, QStringLiteral("/srv/kiosk")).library();
    QCOMPARE(defaults.database, QStringLiteral("/srv/kiosk/library.db"));
    QCOMPARE(defaults.cacheRoot, QStringLiteral("/srv/kiosk/cache"));
    QCOMPARE(defaults.stateDir, QStringLiteral("/srv/kiosk/state"));
    QVERIFY(defaults.renditions.isEmpty());
    QCOMPARE(defaults.jpegQuality, 88);

    const QJsonObject json{{QStringLiteral("library"), QJsonObject{
        {QStringLiteral("cache"), QStringLiteral("/mnt/usb/cache")},
        {QStringLiteral("renditions"), QJsonArray{QStringLiteral("4K"), QStringLiteral("1080p"), QStringLiteral("8k"),
                                                  QStringLiteral("2160p")}},
        {QStringLiteral("jpegQuality"), 120},
    }}};
    const LibrarySettings custom = Config::fromJson(json, QStringLiteral("/srv/kiosk")).library();
    QCOMPARE(custom.cacheRoot, QStringLiteral("/mnt/usb/cache"));
    QCOMPARE(custom.renditions, (QStringList{QStringLiteral("2160p"), QStringLiteral("1080p")}));
    QCOMPARE(custom.jpegQuality, 100);
}

void TestLibrary::fileSnapshot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("library.json"));

    FileSnapshot snapshot(path);
    QVERIFY(!snapshot.changed());

    QVERIFY(writeJsonFile(path, QJsonObject{{QStringLiteral("generation"), 1}}));
    QVERIFY(snapshot.changed());
    QVERIFY(!snapshot.changed());

    QVERIFY(writeJsonFile(path, QJsonObject{{QStringLiteral("generation"), 1}})); // same content
    QVERIFY(!snapshot.changed());
    QVERIFY(writeJsonFile(path, QJsonObject{{QStringLiteral("generation"), 2}}));
    QVERIFY(snapshot.changed());
    QCOMPARE(readJsonFile(path).value(u"generation").toInt(), 2);

    QVERIFY(QFile::remove(path));
    QVERIFY(snapshot.changed());
}

void TestLibrary::syncAndRenditions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString cache = dir.filePath(QStringLiteral("cache"));
    const QString database = dir.filePath(QStringLiteral("library.db"));

    writeImage(media + QStringLiteral("/a.jpg"), QSize(3000, 2000));
    writeImage(media + QStringLiteral("/alpen/b.jpg"), QSize(400, 300));
    writeImage(media + QStringLiteral("/alpen/tief/c.jpg"), QSize(300, 400));
    writeImage(media + QStringLiteral("/alpenrose/d.jpg"), QSize(100, 100));

    Library library(database, media, cache);
    QString error;
    QVERIFY2(library.open(&error), qPrintable(error));

    SyncResult result = library.sync();
    QCOMPARE(result.added, 4);
    QCOMPARE(library.imageCount(), 4);
    result = library.sync();
    QCOMPARE(result.unchanged, 4);
    QCOMPARE(result.added + result.updated + result.removed, 0);

    // Albums include their sub folders, but "alpen" is not a prefix of "alpenrose"
    QList<LibraryEntry> entries;
    QVERIFY(readLibrary(database, media, cache, QStringLiteral("alpen"), &entries, &error));
    QCOMPARE(relativeOriginals(entries, media), (QStringList{QStringLiteral("alpen/b.jpg"), QStringLiteral("alpen/tief/c.jpg")}));
    QVERIFY(readLibrary(database, media, cache, QString(), &entries, &error));
    QCOMPARE(entries.size(), 4);

    // Renditions: newest first, kinds in priority order
    const QStringList kinds{QStringLiteral("1080p"), QStringLiteral("thumb")};
    QCOMPARE(library.pendingCount(kinds), 8);
    const QList<RenditionJob> firstJobs = library.pendingRenditions(kinds, 3);
    QCOMPARE(firstJobs.size(), 3);
    QCOMPARE(firstJobs.constFirst().kind, QStringLiteral("1080p"));
    for (const RenditionJob &job : library.pendingRenditions(kinds, 100))
        QVERIFY2(library.generate(job, 85, &error), qPrintable(error));
    QCOMPARE(library.pendingCount(kinds), 0);

    QVERIFY(readLibrary(database, media, cache, QString(), &entries, &error));
    for (const LibraryEntry &entry : std::as_const(entries)) {
        QCOMPARE(entry.renditions.size(), 2);
        for (const QString &file : entry.renditions)
            QVERIFY2(QFileInfo::exists(file), qPrintable(file));
    }
    const LibraryEntry big = entries.constFirst(); // a.jpg
    QCOMPARE(QImageReader(big.renditions.value(QStringLiteral("1080p"))).size(), QSize(2048, 1365));

    // A changed picture loses its renditions, a sidecar only updates metadata
    writeImage(media + QStringLiteral("/a.jpg"), QSize(1200, 800), Qt::darkRed);
    touchLater(media + QStringLiteral("/a.jpg"));
    QFile sidecar(media + QStringLiteral("/alpen/b.json"));
    QVERIFY(sidecar.open(QIODevice::WriteOnly));
    sidecar.write(R"({ "title": "Bergsee" })");
    sidecar.close();
    touchLater(sidecar.fileName());

    result = library.sync();
    QCOMPARE(result.updated, 2);
    QCOMPARE(library.pendingCount(kinds), 2);
    QVERIFY(!QFileInfo::exists(big.renditions.value(QStringLiteral("1080p"))));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
        db.setDatabaseName(database);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("SELECT title, width, height FROM images ORDER BY path")));
        QVERIFY(query.next()); // a.jpg
        QCOMPARE(query.value(1).toInt(), 1200);
        QVERIFY(query.next()); // alpen/b.jpg
        QCOMPARE(query.value(0).toString(), QStringLiteral("Bergsee"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("check"));

    // Removed pictures take their renditions with them
    const QString cThumb = entries.at(2).renditions.value(QStringLiteral("thumb"));
    QVERIFY(QFileInfo::exists(cThumb));
    QVERIFY(QFile::remove(media + QStringLiteral("/alpen/tief/c.jpg")));
    result = library.sync();
    QCOMPARE(result.removed, 1);
    QVERIFY(!QFileInfo::exists(cThumb));
    QCOMPARE(library.imageCount(), 3);
}

void TestLibrary::brokenFilesAreNotRetried()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    QVERIFY(QDir().mkpath(media));
    QFile broken(media + QStringLiteral("/kaputt.jpg"));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("kein JPEG");
    broken.close();
    writeImage(media + QStringLiteral("/gut.jpg"), QSize(200, 100));

    Library library(dir.filePath(QStringLiteral("library.db")), media, dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 2);

    // Unreadable pictures are indexed but never queued or shown
    const QStringList kinds{QStringLiteral("1080p")};
    QCOMPARE(library.pendingCount(kinds), 1);
    QList<LibraryEntry> entries;
    QVERIFY(readLibrary(dir.filePath(QStringLiteral("library.db")), media, dir.filePath(QStringLiteral("cache")),
                        QString(), &entries));
    QCOMPARE(entries.size(), 1);

    // A rendition that fails is recorded and not tried again
    const RenditionJob job{entries.constFirst().id, media + QStringLiteral("/fehlt.jpg"), QStringLiteral("1080p")};
    QString error;
    QVERIFY(!library.generate(job, 85, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(library.pendingCount(kinds), 0);
}

void TestLibrary::missingMediaRootKeepsLibrary()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    writeImage(media + QStringLiteral("/a.jpg"), QSize(100, 100));

    Library library(dir.filePath(QStringLiteral("library.db")), media, dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 1);

    // e.g. a network share that is not mounted
    QVERIFY(QDir(media).removeRecursively());
    const SyncResult result = library.sync();
    QVERIFY(result.skipped);
    QCOMPARE(library.imageCount(), 1);
}

void TestLibrary::syncDetectsMoves()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString cache = dir.filePath(QStringLiteral("cache"));
    const QString database = dir.filePath(QStringLiteral("library.db"));
    writeImage(media + QStringLiteral("/a.jpg"), QSize(300, 200));
    writeImage(media + QStringLiteral("/alpen/b.jpg"), QSize(200, 300));
    QFile sidecar(media + QStringLiteral("/alpen/b.json"));
    QVERIFY(sidecar.open(QIODevice::WriteOnly));
    sidecar.write(R"({ "title": "Bergsee" })");
    sidecar.close();

    Library library(database, media, cache);
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 2);
    const QStringList kinds{QStringLiteral("1080p"), QStringLiteral("thumb")};
    renderAll(library, kinds);
    const QHash<QString, qint64> before = idsByPath(database, media, cache);
    const qint64 a = before.value(QStringLiteral("a.jpg"));
    QVERIFY(a > 0);

    // Moved by hand (Finder, rsync): rename keeps name, size and time
    QVERIFY(QDir().mkpath(media + QStringLiteral("/wald")));
    QVERIFY(QFile::rename(media + QStringLiteral("/a.jpg"), media + QStringLiteral("/wald/a.jpg")));
    SyncResult result = library.sync();
    QCOMPARE(result.moved, 1);
    QCOMPARE(result.added, 0);
    QCOMPARE(result.removed, 0);
    QHash<QString, qint64> after = idsByPath(database, media, cache);
    QCOMPARE(after.value(QStringLiteral("wald/a.jpg")), a);
    QVERIFY(!after.contains(QStringLiteral("a.jpg")));
    QCOMPARE(library.pendingCount(kinds), 0);
    for (const QString &kind : kinds)
        QVERIFY(QFileInfo::exists(renditionFilePath(cache, a, kind)));

    // Moved through the web UI, together with the sidecar
    const qint64 b = before.value(QStringLiteral("alpen/b.jpg"));
    QString error;
    QVERIFY2(!moveImage(media + QStringLiteral("/alpen/b.jpg"), media + QStringLiteral("/wald"), &error).isEmpty(), qPrintable(error));
    result = library.sync();
    QCOMPARE(result.moved, 1);
    QCOMPARE(result.added + result.removed, 0);
    after = idsByPath(database, media, cache);
    QCOMPARE(after.value(QStringLiteral("wald/b.jpg")), b);
    QCOMPARE(library.pendingCount(kinds), 0);
    QVERIFY(QFileInfo::exists(renditionFilePath(cache, b, QStringLiteral("thumb"))));

    // The album column follows the move, the sidecar still applies
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("moves"));
        db.setDatabaseName(database);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("SELECT album, title FROM images WHERE id = %1").arg(b)));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("wald"));
        QCOMPARE(query.value(1).toString(), QStringLiteral("Bergsee"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("moves"));

    QCOMPARE(library.sync().unchanged, 2);
}

void TestLibrary::movedAlbumKeepsRenditions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString cache = dir.filePath(QStringLiteral("cache"));
    const QString database = dir.filePath(QStringLiteral("library.db"));
    writeImage(media + QStringLiteral("/x/1.jpg"), QSize(100, 100));
    writeImage(media + QStringLiteral("/x/2.jpg"), QSize(120, 100), Qt::darkRed);
    writeImage(media + QStringLiteral("/x/tief/3.jpg"), QSize(100, 140), Qt::darkBlue);

    Library library(database, media, cache);
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 3);
    renderAll(library, {QStringLiteral("thumb")});
    const QHash<QString, qint64> before = idsByPath(database, media, cache);

    // Renaming the album folder moves everything in it
    QVERIFY(QDir().rename(media + QStringLiteral("/x"), media + QStringLiteral("/y")));
    const SyncResult result = library.sync();
    QCOMPARE(result.moved, 3);
    QCOMPARE(result.added + result.removed, 0);
    const QHash<QString, qint64> after = idsByPath(database, media, cache);
    QCOMPARE(after.value(QStringLiteral("y/1.jpg")), before.value(QStringLiteral("x/1.jpg")));
    QCOMPARE(after.value(QStringLiteral("y/2.jpg")), before.value(QStringLiteral("x/2.jpg")));
    QCOMPARE(after.value(QStringLiteral("y/tief/3.jpg")), before.value(QStringLiteral("x/tief/3.jpg")));
    QCOMPARE(library.pendingCount({QStringLiteral("thumb")}), 0);

    QList<LibraryEntry> entries;
    QVERIFY(readLibrary(database, media, cache, QStringLiteral("y"), &entries));
    QCOMPARE(entries.size(), 3);
    QVERIFY(readLibrary(database, media, cache, QStringLiteral("x"), &entries));
    QVERIFY(entries.isEmpty());
}

void TestLibrary::renamedOrChangedFilesAreNotMoves()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString cache = dir.filePath(QStringLiteral("cache"));
    const QString database = dir.filePath(QStringLiteral("library.db"));
    writeImage(media + QStringLiteral("/a.jpg"), QSize(100, 100));
    writeImage(media + QStringLiteral("/c.jpg"), QSize(100, 100), Qt::darkRed);

    Library library(database, media, cache);
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 2);
    renderAll(library, {QStringLiteral("thumb")});
    const QHash<QString, qint64> before = idsByPath(database, media, cache);
    const QString oldThumb = renditionFilePath(cache, before.value(QStringLiteral("a.jpg")), QStringLiteral("thumb"));
    QVERIFY(QFileInfo::exists(oldThumb));

    // A new name is a new picture
    QVERIFY(QFile::rename(media + QStringLiteral("/a.jpg"), media + QStringLiteral("/z.jpg")));
    SyncResult result = library.sync();
    QCOMPARE(result.moved, 0);
    QCOMPARE(result.added, 1);
    QCOMPARE(result.removed, 1);
    QVERIFY(idsByPath(database, media, cache).value(QStringLiteral("z.jpg")) != before.value(QStringLiteral("a.jpg")));
    QVERIFY(!QFileInfo::exists(oldThumb));

    // Same name in another folder, but different content: not a move either
    QVERIFY(QFile::remove(media + QStringLiteral("/c.jpg")));
    writeImage(media + QStringLiteral("/anders/c.jpg"), QSize(400, 300), Qt::darkGreen);
    result = library.sync();
    QCOMPARE(result.moved, 0);
    QCOMPARE(result.added, 1);
    QCOMPARE(result.removed, 1);
    QVERIFY(idsByPath(database, media, cache).value(QStringLiteral("anders/c.jpg")) != before.value(QStringLiteral("c.jpg")));
}

void TestLibrary::renditionFilePathOnlyKnownKinds()
{
    const QString cache = QStringLiteral("/srv/kiosk/cache");
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("thumb")), QStringLiteral("/srv/kiosk/cache/thumb/7.jpg"));
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("1080p")), QStringLiteral("/srv/kiosk/cache/1080p/7.jpg"));
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("2160p")), QStringLiteral("/srv/kiosk/cache/2160p/7.jpg"));
    // The kind becomes a folder name: never a path
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("../x")), QString());
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("../../etc")), QString());
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("/etc")), QString());
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("foo")), QString());
    QCOMPARE(renditionFilePath(cache, 7, QStringLiteral("original")), QString());
    QCOMPARE(renditionFilePath(cache, 7, QString()), QString());
}

void TestLibrary::scanIgnoresSymlinks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString outside = dir.filePath(QStringLiteral("draussen"));
    writeImage(media + QStringLiteral("/a.jpg"), QSize(40, 30));
    writeImage(outside + QStringLiteral("/geheim.jpg"), QSize(40, 30));
    writeImage(outside + QStringLiteral("/ordner/b.jpg"), QSize(40, 30));

    // A linked file and a linked folder inside the media folder, both leading out of it
    QVERIFY(QFile::link(outside + QStringLiteral("/geheim.jpg"), media + QStringLiteral("/link.jpg")));
    QVERIFY(QFile::link(outside + QStringLiteral("/ordner"), media + QStringLiteral("/linkordner")));

    QStringList found = scanImages(media);
    for (QString &path : found)
        path = QDir(media).relativeFilePath(path);
    QCOMPARE(found, QStringList{QStringLiteral("a.jpg")});

    Library library(dir.filePath(QStringLiteral("library.db")), media, dir.filePath(QStringLiteral("cache")));
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 1);
    QCOMPARE(library.imageCount(), 1);
}

void TestLibrary::albumDirectoryRejectsSymlinkEscape()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString outside = dir.filePath(QStringLiteral("draussen"));
    QVERIFY(QDir().mkpath(media + QStringLiteral("/echt/unter")));
    QVERIFY(QDir().mkpath(outside + QStringLiteral("/tief")));
    QVERIFY(QFile::link(outside, media + QStringLiteral("/fluchtweg")));
    // A link that stays inside the media folder is harmless
    QVERIFY(QFile::link(media + QStringLiteral("/echt"), media + QStringLiteral("/verweis")));

    // The names are fine, but the existing part of the way leads outside
    QCOMPARE(albumDirectory(media, QStringLiteral("fluchtweg")), QString());
    QCOMPARE(albumDirectory(media, QStringLiteral("fluchtweg/tief")), QString());
    QCOMPARE(albumDirectory(media, QStringLiteral("fluchtweg/neu/noch-tiefer")), QString());

    QCOMPARE(albumDirectory(media, QString()), QDir::cleanPath(media));
    QCOMPARE(albumDirectory(media, QStringLiteral("echt/unter")), QDir::cleanPath(media + QStringLiteral("/echt/unter")));
    QCOMPARE(albumDirectory(media, QStringLiteral("echt/neu/tiefer")),
             QDir::cleanPath(media + QStringLiteral("/echt/neu/tiefer")));
    QCOMPARE(albumDirectory(media, QStringLiteral("ganz/neu")), QDir::cleanPath(media + QStringLiteral("/ganz/neu")));
    QCOMPARE(albumDirectory(media, QStringLiteral("verweis/unter")), QDir::cleanPath(media + QStringLiteral("/verweis/unter")));
}

void TestLibrary::failedRenditionIsRecordedAndRetriedAfterSuccess()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = dir.filePath(QStringLiteral("media"));
    const QString database = dir.filePath(QStringLiteral("library.db"));
    const QString cache = dir.filePath(QStringLiteral("cache"));
    const QString image = media + QStringLiteral("/bild.jpg");
    writeImage(image, QSize(80, 60));

    Library library(database, media, cache);
    QVERIFY(library.open());
    QCOMPARE(library.sync().added, 1);
    const QStringList kinds{QStringLiteral("thumb")};
    const QList<RenditionJob> jobs = library.pendingRenditions(kinds, 10);
    QCOMPARE(jobs.size(), 1);

    const auto row = [&database](qint64 id, QString *path, QVariant *error) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("probe"));
        db.setDatabaseName(database);
        bool found = false;
        if (db.open()) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT path, error FROM renditions WHERE image_id = ? AND kind = 'thumb'"));
            query.addBindValue(id);
            if (query.exec() && query.next()) {
                *path = query.value(0).toString();
                *error = query.value(1);
                found = true;
            }
        }
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("probe"));
        return found;
    };

    // The file turns bad after the sync: generating fails and is recorded
    QFile bad(image);
    QVERIFY(bad.open(QIODevice::WriteOnly | QIODevice::Truncate));
    bad.write("kein Bild mehr");
    bad.close();
    QString error;
    QVERIFY(!library.generate(jobs.first(), 80, &error));
    QVERIFY(!error.isEmpty());
    QString path;
    QVariant recorded;
    QVERIFY(row(jobs.first().imageId, &path, &recorded));
    QCOMPARE(path, QString());
    QVERIFY(!recorded.isNull());
    QVERIFY(!recorded.toString().isEmpty());
    QCOMPARE(library.pendingCount(kinds), 0); // not retried in a loop

    // Once the file is fine again, a successful run replaces the failure
    writeImage(image, QSize(80, 60));
    QVERIFY2(library.generate(jobs.first(), 80, &error), qPrintable(error));
    QVERIFY(row(jobs.first().imageId, &path, &recorded));
    QCOMPARE(path, QStringLiteral("thumb/%1.jpg").arg(jobs.first().imageId));
    QVERIFY(recorded.isNull());
    QVERIFY(QFileInfo::exists(renditionFilePath(cache, jobs.first().imageId, QStringLiteral("thumb"))));
}

QTEST_GUILESS_MAIN(TestLibrary)
#include "tst_library.moc"
