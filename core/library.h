#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class QSqlDatabase;

namespace berry {

struct SyncResult
{
    int added = 0;
    int updated = 0;
    int removed = 0;
    int unchanged = 0;
    bool skipped = false; // media root missing, library left alone
};

struct RenditionJob
{
    qint64 imageId = 0;
    QString source; // absolute path of the original
    QString kind;
};

struct LibraryEntry
{
    qint64 id = 0;
    QString original;                   // absolute path
    QHash<QString, QString> renditions; // kind -> absolute path
};

// The media library: an index of all pictures below the media root in SQLite,
// with their metadata and the renditions in the cache. The media folder stays
// the source of truth; sync() brings the index up to date with it.
// Written by the admin process only. One instance per thread.
class Library
{
public:
    Library(const QString &databasePath, const QString &mediaRoot, const QString &cacheRoot);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    // Creates or migrates the database
    bool open(QString *error = nullptr);

    SyncResult sync();

    // Newest pictures first, kinds in the given order of priority
    QList<RenditionJob> pendingRenditions(const QStringList &kinds, int limit) const;
    int pendingCount(const QStringList &kinds) const;
    // Creates the rendition and records it. A failure is recorded as well, so a
    // broken file is not retried until it changes.
    bool generate(const RenditionJob &job, int quality, QString *error = nullptr);

    int imageCount() const;

private:
    QSqlDatabase database() const;
    void writeImage(qint64 id, const QString &file, const QString &relative, qint64 size, qint64 modified,
                    qint64 sidecarModified);
    void removeRenditions(qint64 imageId);

    QString m_databasePath;
    QString m_mediaRoot;
    QString m_cacheRoot;
    QString m_connectionName;
};

// Read side for the display: the enabled, readable pictures of an album
// (including its sub folders; empty = all) with their existing renditions.
// Opens the database read-only, so it is safe while the admin process writes.
// Returns false when there is no usable library.
bool readLibrary(const QString &databasePath, const QString &mediaRoot, const QString &cacheRoot,
                 const QString &album, QList<LibraryEntry> *entries, QString *error = nullptr);

// Where a rendition lives (whether or not it exists yet)
QString renditionFilePath(const QString &cacheRoot, qint64 imageId, const QString &kind);

// Read side for the admin API, read-only like readLibrary. Media objects carry
// id, path, album, width, height, enabled, addedAt, renditions (kinds) and,
// where known, title, description, location, camera, lens, taken, aperture,
// exposureTime, focalLength, iso, quote and quoteAuthor.
struct MediaQuery
{
    QString album; // including sub folders, '' = all
    int offset = 0;
    int limit = 100;
};
QJsonObject queryMedia(const QString &databasePath, const MediaQuery &query, QString *error = nullptr); // { total, items }
QJsonObject mediaById(const QString &databasePath, qint64 id, QString *error = nullptr); // empty when unknown
qint64 mediaIdByPath(const QString &databasePath, const QString &relativePath);         // 0 when unknown
QJsonArray albumCounts(const QString &databasePath, QString *error = nullptr); // [{ name, count, enabled }]

} // namespace berry
