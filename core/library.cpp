#include "library.h"

#include "imagemetadata.h"
#include "imagescanner.h"
#include "renditions.h"

#include <QAtomicInteger>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace berry {

namespace {

constexpr int kSchemaVersion = 1;
constexpr int kCommitEvery = 100;

const char *const kSchema[] = {
    R"(CREATE TABLE images (
        id INTEGER PRIMARY KEY,
        path TEXT NOT NULL UNIQUE,          -- relative to the media root, '/' separated
        album TEXT NOT NULL,                -- folder of path, '' for the root
        file_size INTEGER NOT NULL,
        file_mtime INTEGER NOT NULL,        -- ms since the epoch
        sidecar_mtime INTEGER NOT NULL DEFAULT 0,
        width INTEGER NOT NULL DEFAULT 0,   -- upright, 0 = unreadable
        height INTEGER NOT NULL DEFAULT 0,
        title TEXT,
        description TEXT,
        location TEXT,
        camera TEXT,
        lens TEXT,
        taken TEXT,                         -- ISO 8601, local time of the camera
        aperture REAL,
        exposure_time REAL,
        focal_length REAL,
        iso INTEGER,
        quote TEXT,
        quote_author TEXT,
        enabled INTEGER NOT NULL DEFAULT 1,
        added_at TEXT NOT NULL
    ))",
    "CREATE INDEX images_album ON images (album)",
    R"(CREATE TABLE renditions (
        image_id INTEGER NOT NULL REFERENCES images (id) ON DELETE CASCADE,
        kind TEXT NOT NULL,                 -- '1080p', '2160p', 'thumb'
        path TEXT NOT NULL,                 -- relative to the cache root, '' when it failed
        width INTEGER NOT NULL DEFAULT 0,
        height INTEGER NOT NULL DEFAULT 0,
        error TEXT,
        PRIMARY KEY (image_id, kind)
    ))",
};

QString uniqueConnectionName(QLatin1String prefix)
{
    static QAtomicInteger<quint64> counter;
    return QStringLiteral("berry-%1-%2").arg(prefix).arg(++counter);
}

bool run(QSqlQuery &query, const QString &sql = {})
{
    const bool ok = sql.isEmpty() ? query.exec() : query.exec(sql);
    if (!ok)
        qWarning().noquote() << "Mediathek:" << query.lastError().text();
    return ok;
}

qint64 modifiedMsecs(const QFileInfo &info)
{
    return info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
}

// Qt binds a null QString as SQL NULL; album columns and parameters need ''
QString notNull(const QString &value)
{
    return value.isNull() ? QString(u"") : value;
}

QString albumOf(const QString &relativePath)
{
    const QString dir = QFileInfo(relativePath).path();
    return notNull(dir == u"." ? QString() : dir);
}

QVariant nullable(const QString &value)
{
    return value.isEmpty() ? QVariant() : QVariant(value);
}

QVariant nullable(double value)
{
    return value > 0 ? QVariant(value) : QVariant();
}

QString renditionPath(qint64 imageId, const QString &kind)
{
    return QStringLiteral("%1/%2.jpg").arg(kind).arg(imageId);
}

} // namespace

Library::Library(const QString &databasePath, const QString &mediaRoot, const QString &cacheRoot)
    : m_databasePath(databasePath)
    , m_mediaRoot(QDir::cleanPath(mediaRoot))
    , m_cacheRoot(QDir::cleanPath(cacheRoot))
    , m_connectionName(uniqueConnectionName(QLatin1String("library")))
{
}

Library::~Library()
{
    if (!QSqlDatabase::contains(m_connectionName))
        return;
    database().close();
    QSqlDatabase::removeDatabase(m_connectionName);
}

QSqlDatabase Library::database() const
{
    return QSqlDatabase::database(m_connectionName, false);
}

bool Library::open(QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    QDir().mkpath(QFileInfo(m_databasePath).absolutePath());
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(m_databasePath);
    if (!db.open())
        return fail(db.lastError().text());

    QSqlQuery query(db);
    // WAL lets the display read while we write
    run(query, QStringLiteral("PRAGMA journal_mode = WAL"));
    run(query, QStringLiteral("PRAGMA foreign_keys = ON"));
    run(query, QStringLiteral("PRAGMA busy_timeout = 5000"));

    if (!run(query, QStringLiteral("PRAGMA user_version")) || !query.next())
        return fail(query.lastError().text());
    const int version = query.value(0).toInt();
    if (version > kSchemaVersion)
        return fail(QStringLiteral("Mediathek stammt aus einer neueren Version (Schema %1)").arg(version));

    if (version < 1) {
        db.transaction();
        for (const char *statement : kSchema) {
            if (!run(query, QString::fromUtf8(statement))) {
                db.rollback();
                return fail(query.lastError().text());
            }
        }
        run(query, QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion));
        db.commit();
    }
    return true;
}

SyncResult Library::sync()
{
    SyncResult result;
    // An unmounted network share must not empty the library
    if (!QFileInfo(m_mediaRoot).isDir()) {
        qWarning().noquote() << "Bilderordner fehlt, Mediathek bleibt unverändert:" << m_mediaRoot;
        result.skipped = true;
        return result;
    }

    QSqlDatabase db = database();
    struct Known
    {
        qint64 id;
        qint64 size;
        qint64 modified;
        qint64 sidecarModified;
    };
    QHash<QString, Known> known;
    {
        QSqlQuery query(db);
        run(query, QStringLiteral("SELECT id, path, file_size, file_mtime, sidecar_mtime FROM images"));
        while (query.next()) {
            known.insert(query.value(1).toString(),
                         {query.value(0).toLongLong(), query.value(2).toLongLong(), query.value(3).toLongLong(),
                          query.value(4).toLongLong()});
        }
    }

    const QDir root(m_mediaRoot);
    int uncommitted = 0;
    // Commit in batches so a long first sync shows up bit by bit
    const auto wrote = [&] {
        if (++uncommitted >= kCommitEvery) {
            db.commit();
            db.transaction();
            uncommitted = 0;
        }
    };

    struct Fresh
    {
        QString file;
        QString relative;
        qint64 size;
        qint64 modified;
        qint64 sidecarModified;
    };
    QList<Fresh> fresh;

    db.transaction();
    for (const QString &file : scanImages(m_mediaRoot)) {
        const QString relative = root.relativeFilePath(file);
        const QFileInfo info(file);
        const qint64 size = info.size();
        const qint64 modified = modifiedMsecs(info);
        const qint64 sidecarModified = modifiedMsecs(QFileInfo(sidecarPath(file)));

        const auto it = known.constFind(relative);
        if (it == known.cend()) {
            fresh.append({file, relative, size, modified, sidecarModified});
            continue;
        }
        const Known image = *it;
        known.erase(it);
        if (image.size != size || image.modified != modified) {
            removeRenditions(image.id); // new pixels, new renditions
            writeImage(image.id, file, relative, size, modified, sidecarModified);
            ++result.updated;
        } else if (image.sidecarModified != sidecarModified) {
            writeImage(image.id, file, relative, size, modified, sidecarModified);
            ++result.updated;
        } else {
            ++result.unchanged;
            continue;
        }
        wrote();
    }

    // A file that vanished in one place and turned up in another with the same
    // name, size and modification time was moved (Finder, rsync, the web UI):
    // it keeps its id and, above all, its renditions.
    const auto moveKey = [](const QString &path, qint64 size, qint64 modified) {
        return QStringLiteral("%1|%2|%3").arg(QFileInfo(path).fileName()).arg(size).arg(modified);
    };
    QHash<QString, QString> vanished; // move key -> old relative path
    for (auto it = known.cbegin(); it != known.cend(); ++it)
        vanished.insert(moveKey(it.key(), it->size, it->modified), it.key());

    for (const Fresh &image : std::as_const(fresh)) {
        const QString key = moveKey(image.relative, image.size, image.modified);
        const auto from = vanished.constFind(key);
        if (from != vanished.cend() && known.contains(*from)) {
            writeImage(known.value(*from).id, image.file, image.relative, image.size, image.modified,
                       image.sidecarModified);
            known.remove(*from);
            vanished.erase(from);
            ++result.moved;
        } else {
            writeImage(0, image.file, image.relative, image.size, image.modified, image.sidecarModified);
            ++result.added;
        }
        wrote();
    }

    QSqlQuery remove(db);
    remove.prepare(QStringLiteral("DELETE FROM images WHERE id = ?"));
    for (const Known &image : std::as_const(known)) {
        removeRenditions(image.id);
        remove.addBindValue(image.id);
        run(remove);
        ++result.removed;
    }
    db.commit();
    return result;
}

void Library::writeImage(qint64 id, const QString &file, const QString &relative, qint64 size, qint64 modified,
                         qint64 sidecarModified)
{
    const ImageMetadata m = readImageMetadata(file);
    const QSize dimensions = uprightImageSize(file);

    QSqlQuery query(database());
    if (id == 0) {
        query.prepare(QStringLiteral(
            "INSERT INTO images (path, album, file_size, file_mtime, sidecar_mtime, width, height, title, "
            "description, location, camera, lens, taken, aperture, exposure_time, focal_length, iso, quote, "
            "quote_author, enabled, added_at) "
            "VALUES (:path, :album, :size, :mtime, :sidecar, :width, :height, :title, :description, :location, "
            ":camera, :lens, :taken, :aperture, :exposure, :focal, :iso, :quote, :author, :enabled, :added)"));
        query.bindValue(QStringLiteral(":added"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    } else {
        query.prepare(QStringLiteral(
            "UPDATE images SET path = :path, album = :album, file_size = :size, file_mtime = :mtime, "
            "sidecar_mtime = :sidecar, width = :width, height = :height, title = :title, "
            "description = :description, location = :location, camera = :camera, lens = :lens, taken = :taken, "
            "aperture = :aperture, exposure_time = :exposure, focal_length = :focal, iso = :iso, quote = :quote, "
            "quote_author = :author, enabled = :enabled WHERE id = :id"));
        query.bindValue(QStringLiteral(":id"), id);
    }
    query.bindValue(QStringLiteral(":path"), relative);
    query.bindValue(QStringLiteral(":album"), albumOf(relative));
    query.bindValue(QStringLiteral(":size"), size);
    query.bindValue(QStringLiteral(":mtime"), modified);
    query.bindValue(QStringLiteral(":sidecar"), sidecarModified);
    query.bindValue(QStringLiteral(":width"), dimensions.width());
    query.bindValue(QStringLiteral(":height"), dimensions.height());
    query.bindValue(QStringLiteral(":title"), nullable(m.title));
    query.bindValue(QStringLiteral(":description"), nullable(m.description));
    query.bindValue(QStringLiteral(":location"), nullable(m.location));
    query.bindValue(QStringLiteral(":camera"), nullable(m.camera));
    query.bindValue(QStringLiteral(":lens"), nullable(m.lens));
    query.bindValue(QStringLiteral(":taken"),
                    m.taken.isValid() ? QVariant(m.taken.toString(Qt::ISODate)) : QVariant());
    query.bindValue(QStringLiteral(":aperture"), nullable(m.aperture));
    query.bindValue(QStringLiteral(":exposure"), nullable(m.exposureTime));
    query.bindValue(QStringLiteral(":focal"), nullable(m.focalLength));
    query.bindValue(QStringLiteral(":iso"), m.iso > 0 ? QVariant(m.iso) : QVariant());
    query.bindValue(QStringLiteral(":quote"), nullable(m.quote));
    query.bindValue(QStringLiteral(":author"), nullable(m.quoteAuthor));
    query.bindValue(QStringLiteral(":enabled"), m.enabled ? 1 : 0);
    run(query);
}

void Library::removeRenditions(qint64 imageId)
{
    const QDir cache(m_cacheRoot);
    QSqlQuery query(database());
    query.prepare(QStringLiteral("SELECT path FROM renditions WHERE image_id = ? AND path <> ''"));
    query.addBindValue(imageId);
    run(query);
    while (query.next())
        QFile::remove(cache.filePath(query.value(0).toString()));

    query.prepare(QStringLiteral("DELETE FROM renditions WHERE image_id = ?"));
    query.addBindValue(imageId);
    run(query);
}

QList<RenditionJob> Library::pendingRenditions(const QStringList &kinds, int limit) const
{
    QList<RenditionJob> jobs;
    const QDir root(m_mediaRoot);
    QSqlQuery query(database());
    for (const QString &kind : kinds) {
        if (jobs.size() >= limit)
            break;
        query.prepare(QStringLiteral(
            "SELECT id, path FROM images WHERE width > 0 AND NOT EXISTS "
            "(SELECT 1 FROM renditions r WHERE r.image_id = images.id AND r.kind = ?) "
            "ORDER BY added_at DESC, id DESC LIMIT ?"));
        query.addBindValue(kind);
        query.addBindValue(limit - jobs.size());
        run(query);
        while (query.next())
            jobs.append({query.value(0).toLongLong(), root.filePath(query.value(1).toString()), kind});
    }
    return jobs;
}

int Library::pendingCount(const QStringList &kinds) const
{
    int count = 0;
    QSqlQuery query(database());
    for (const QString &kind : kinds) {
        query.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM images WHERE width > 0 AND NOT EXISTS "
            "(SELECT 1 FROM renditions r WHERE r.image_id = images.id AND r.kind = ?)"));
        query.addBindValue(kind);
        if (run(query) && query.next())
            count += query.value(0).toInt();
    }
    return count;
}

bool Library::generate(const RenditionJob &job, int quality, QString *error)
{
    const QString relative = renditionPath(job.imageId, job.kind);
    QSize size;
    QString message;

    // Recorded as failed before decoding: a file that crashes the decoder
    // (or gets the service killed for memory) is not tried again forever
    {
        QSqlQuery mark(database());
        mark.prepare(QStringLiteral("INSERT OR REPLACE INTO renditions (image_id, kind, path, width, height, error) "
                                    "VALUES (?, ?, '', 0, 0, ?)"));
        mark.addBindValue(job.imageId);
        mark.addBindValue(job.kind);
        mark.addBindValue(QStringLiteral("Abbruch beim Erstellen"));
        run(mark);
    }

    const bool ok = createRendition(job.source, QDir(m_cacheRoot).filePath(relative), job.kind, quality, &size,
                                    &message);

    QSqlQuery query(database());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO renditions (image_id, kind, path, width, height, error) VALUES (?, ?, ?, ?, ?, ?)"));
    query.addBindValue(job.imageId);
    query.addBindValue(job.kind);
    query.addBindValue(ok ? relative : QString(u""));
    query.addBindValue(ok ? size.width() : 0);
    query.addBindValue(ok ? size.height() : 0);
    query.addBindValue(ok ? QVariant() : QVariant(message));
    run(query);

    if (!ok && error)
        *error = message;
    return ok;
}

int Library::imageCount() const
{
    QSqlQuery query(database());
    return run(query, QStringLiteral("SELECT COUNT(*) FROM images")) && query.next() ? query.value(0).toInt() : 0;
}


QString renditionFilePath(const QString &cacheRoot, qint64 imageId, const QString &kind)
{
    // kind becomes a folder name: only the known ones, never a path
    if (!isRenditionKind(kind))
        return {};
    return QDir(cacheRoot).filePath(renditionPath(imageId, kind));
}

namespace {

// A read-only connection for one call; safe next to the writing admin worker
class ReadOnlyDatabase
{
public:
    explicit ReadOnlyDatabase(const QString &path)
        : m_name(uniqueConnectionName(QLatin1String("read")))
    {
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        m_database.setDatabaseName(path);
        m_database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
        m_exists = QFileInfo::exists(path);
    }

    ~ReadOnlyDatabase()
    {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }

    bool open(QString *error)
    {
        if (!m_exists) {
            if (error)
                *error = QStringLiteral("keine Mediathek unter %1").arg(m_database.databaseName());
            return false;
        }
        if (m_database.open())
            return true;
        if (error)
            *error = m_database.lastError().text();
        return false;
    }

    const QSqlDatabase &database() const { return m_database; }

private:
    QString m_name;
    QSqlDatabase m_database;
    bool m_exists = false;
};

// For LIKE ... ESCAPE '\': the search text is matched literally
QString escapeLike(QString text)
{
    return text.replace(u'\\', QStringLiteral("\\\\")).replace(u'%', QStringLiteral("\\%")).replace(u'_', QStringLiteral("\\_"));
}

struct AlbumFilter
{
    QString album;      // '' = everything
    QString likePrefix; // for sub folders, LIKE-escaped
};

AlbumFilter albumFilter(const QString &album)
{
    QString clean = QDir::cleanPath(album);
    if (clean == u"." || clean == u"/")
        clean.clear();
    clean = notNull(clean);
    return {clean, escapeLike(clean) + QStringLiteral("/%")};
}

constexpr auto kAlbumCondition = "(? = '' OR album = ? OR album LIKE ? ESCAPE '\\')";

void bindAlbum(QSqlQuery &query, const AlbumFilter &filter)
{
    query.addBindValue(filter.album);
    query.addBindValue(filter.album);
    query.addBindValue(filter.likePrefix);
}

constexpr auto kMediaColumns =
    "id, path, album, width, height, title, description, location, camera, lens, taken, aperture, "
    "exposure_time, focal_length, iso, quote, quote_author, enabled, added_at, "
    "(SELECT group_concat(kind) FROM renditions r WHERE r.image_id = images.id AND r.path <> ''), "
    "file_size, file_mtime";

QJsonObject mediaFromQuery(const QSqlQuery &query)
{
    QJsonObject media{
        {QStringLiteral("id"), query.value(0).toLongLong()},
        {QStringLiteral("path"), query.value(1).toString()},
        {QStringLiteral("album"), query.value(2).toString()},
        {QStringLiteral("width"), query.value(3).toInt()},
        {QStringLiteral("height"), query.value(4).toInt()},
        {QStringLiteral("enabled"), query.value(17).toInt() != 0},
        {QStringLiteral("addedAt"), query.value(18).toString()},
        {QStringLiteral("renditions"),
         QJsonArray::fromStringList(query.value(19).toString().split(u',', Qt::SkipEmptyParts))},
        {QStringLiteral("fileSize"), query.value(20).toLongLong()},
        // Changes whenever the file does; clients put it into image URLs to bypass caches
        {QStringLiteral("version"), query.value(21).toLongLong()},
    };
    const auto text = [&](int column, const char *key) {
        if (!query.isNull(column))
            media.insert(QLatin1String(key), query.value(column).toString());
    };
    // EXIF values come as floats: 5.6 would read 5.599999904632568 otherwise
    const auto number = [&](int column, const char *key) {
        if (!query.isNull(column))
            media.insert(QLatin1String(key), QString::number(query.value(column).toDouble(), 'g', 6).toDouble());
    };
    text(5, "title");
    text(6, "description");
    text(7, "location");
    text(8, "camera");
    text(9, "lens");
    text(10, "taken");
    number(11, "aperture");
    number(12, "exposureTime");
    number(13, "focalLength");
    number(14, "iso");
    text(15, "quote");
    text(16, "quoteAuthor");
    return media;
}

} // namespace

bool readLibrary(const QString &databasePath, const QString &mediaRoot, const QString &cacheRoot,
                 const QString &album, QList<LibraryEntry> *entries, QString *error)
{
    entries->clear();
    ReadOnlyDatabase db(databasePath);
    if (!db.open(error))
        return false;

    QSqlQuery query(db.database());
    query.prepare(QStringLiteral(
        "SELECT i.id, i.path, r.kind, r.path FROM images i "
        "LEFT JOIN renditions r ON r.image_id = i.id AND r.path <> '' "
        "WHERE i.enabled = 1 AND i.width > 0 AND (? = '' OR i.album = ? OR i.album LIKE ? ESCAPE '\\') "
        "ORDER BY i.path COLLATE NOCASE, i.id"));
    bindAlbum(query, albumFilter(album));
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return false;
    }

    const QDir media(mediaRoot);
    const QDir cache(cacheRoot);
    while (query.next()) {
        const qint64 id = query.value(0).toLongLong();
        if (entries->isEmpty() || entries->constLast().id != id)
            entries->append({id, media.filePath(query.value(1).toString()), {}});
        if (!query.isNull(2))
            entries->last().renditions.insert(query.value(2).toString(), cache.filePath(query.value(3).toString()));
    }
    return true;
}

QJsonObject queryMedia(const QString &databasePath, const MediaQuery &request, QString *error)
{
    ReadOnlyDatabase db(databasePath);
    if (!db.open(error))
        return {};

    // The same WHERE clause for the count and for the page
    const AlbumFilter filter = albumFilter(request.album);
    QStringList conditions{QLatin1String(kAlbumCondition)};
    QVariantList values{filter.album, filter.album, filter.likePrefix};
    if (request.enabled.has_value()) {
        conditions.append(QStringLiteral("enabled = ?"));
        values.append(*request.enabled ? 1 : 0);
    }
    const QString search = request.search.trimmed();
    if (!search.isEmpty()) {
        // SQLite's LIKE ignores case for ASCII letters only: "a" finds "A", but "ä" not "Ä"
        conditions.append(QStringLiteral("(title LIKE ? ESCAPE '\\' OR location LIKE ? ESCAPE '\\' "
                                         "OR description LIKE ? ESCAPE '\\' OR path LIKE ? ESCAPE '\\')"));
        const QString pattern = u'%' + escapeLike(search) + u'%';
        values << pattern << pattern << pattern << pattern;
    }
    const QString where = conditions.join(QStringLiteral(" AND "));
    const auto bindAll = [&values](QSqlQuery &query) {
        for (const QVariant &value : values)
            query.addBindValue(value);
    };

    QSqlQuery query(db.database());
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM images WHERE %1").arg(where));
    bindAll(query);
    if (!query.exec() || !query.next()) {
        if (error)
            *error = query.lastError().text();
        return {};
    }
    const int total = query.value(0).toInt();

    const QString direction = request.ascending ? QStringLiteral("ASC") : QStringLiteral("DESC");
    QString order;
    if (request.sort == u"taken")
        order = QStringLiteral("taken IS NULL, taken %1, id %1").arg(direction); // undated pictures last
    else if (request.sort == u"name")
        order = QStringLiteral("path COLLATE NOCASE %1, id %1").arg(direction);
    else
        order = QStringLiteral("added_at %1, id %1").arg(direction);

    query.prepare(QStringLiteral("SELECT %1 FROM images WHERE %2 ORDER BY %3 LIMIT ? OFFSET ?")
                      .arg(QLatin1String(kMediaColumns), where, order));
    bindAll(query);
    query.addBindValue(qBound(1, request.limit, 500));
    query.addBindValue(qMax(0, request.offset));
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return {};
    }
    QJsonArray items;
    while (query.next())
        items.append(mediaFromQuery(query));
    return {{QStringLiteral("total"), total}, {QStringLiteral("items"), items}};
}

QJsonObject mediaById(const QString &databasePath, qint64 id, QString *error)
{
    ReadOnlyDatabase db(databasePath);
    if (!db.open(error))
        return {};
    QSqlQuery query(db.database());
    query.prepare(QStringLiteral("SELECT %1 FROM images WHERE id = ?").arg(QLatin1String(kMediaColumns)));
    query.addBindValue(id);
    if (!query.exec()) {
        if (error)
            *error = query.lastError().text();
        return {};
    }
    return query.next() ? mediaFromQuery(query) : QJsonObject();
}

qint64 mediaIdByPath(const QString &databasePath, const QString &relativePath)
{
    ReadOnlyDatabase db(databasePath);
    if (!db.open(nullptr))
        return 0;
    QSqlQuery query(db.database());
    query.prepare(QStringLiteral("SELECT id FROM images WHERE path = ?"));
    query.addBindValue(relativePath);
    return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
}

QJsonArray albumCounts(const QString &databasePath, QString *error)
{
    ReadOnlyDatabase db(databasePath);
    if (!db.open(error))
        return {};
    QSqlQuery query(db.database());
    // The newest picture of each album serves as its cover
    if (!query.exec(QStringLiteral(
            "SELECT a.album, a.count, a.enabled, i.id, i.file_mtime FROM "
            "(SELECT album, COUNT(*) AS count, SUM(enabled) AS enabled, MAX(id) AS newest FROM images GROUP BY album) a "
            "JOIN images i ON i.id = a.newest ORDER BY a.album COLLATE NOCASE"))) {
        if (error)
            *error = query.lastError().text();
        return {};
    }
    QJsonArray albums;
    while (query.next()) {
        albums.append(QJsonObject{
            {QStringLiteral("name"), query.value(0).toString()},
            {QStringLiteral("count"), query.value(1).toInt()},
            {QStringLiteral("enabled"), query.value(2).toInt()},
            {QStringLiteral("cover"), QJsonObject{{QStringLiteral("id"), query.value(3).toLongLong()},
                                                  {QStringLiteral("version"), query.value(4).toLongLong()}}},
        });
    }
    return albums;
}

} // namespace berry
