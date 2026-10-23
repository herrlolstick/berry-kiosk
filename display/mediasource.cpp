#include "mediasource.h"

#include "imagescanner.h"
#include "library.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRandomGenerator>

#include <algorithm>

MediaSource::MediaSource(QObject *parent)
    : QObject(parent)
{
    m_rescanTimer.setSingleShot(true);
    connect(&m_rescanTimer, &QTimer::timeout, this, &MediaSource::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &MediaSource::onDirectoryChanged);

    // Network shares do not always report changes, so look again now and then
    m_periodicRescan.setInterval(10 * 60 * 1000);
    connect(&m_periodicRescan, &QTimer::timeout, this, &MediaSource::rescan);
    m_periodicRescan.start();
}

void MediaSource::setRoot(const QString &root)
{
    if (m_root == root)
        return;
    m_root = root;
    emit rootChanged();
    emit directoryChanged();
    scheduleRescan(0);
}

void MediaSource::setAlbum(const QString &album)
{
    if (m_album == album)
        return;
    m_album = album;
    emit albumChanged();
    emit directoryChanged();
    scheduleRescan(0);
}

void MediaSource::setOrder(const QString &order)
{
    if (m_order == order)
        return;
    m_order = order;
    m_shuffleQueue.clear();
    emit orderChanged();
}

void MediaSource::setDatabase(const QString &database)
{
    if (m_database == database)
        return;
    m_database = database;
    emit databaseChanged();
    scheduleRescan(0);
}

void MediaSource::setCacheRoot(const QString &cacheRoot)
{
    if (m_cacheRoot == cacheRoot)
        return;
    m_cacheRoot = cacheRoot;
    emit cacheRootChanged();
    scheduleRescan(0);
}

void MediaSource::setStateDir(const QString &stateDir)
{
    if (m_stateDir == stateDir)
        return;
    m_stateDir = stateDir;
    m_libraryState.setPath(stateDir.isEmpty() ? QString() : berry::libraryStatePath(stateDir));
    emit stateDirChanged();
    scheduleRescan(0);
}

void MediaSource::setTier(const QString &tier)
{
    if (m_tier == tier)
        return;
    m_tier = tier; // picked per image in next(), no rescan needed
    emit tierChanged();
}

QString MediaSource::directory() const
{
    if (m_root.isEmpty())
        return {};
    const QString root = QDir::cleanPath(m_root);
    const QString dir = QDir::cleanPath(root + u'/' + m_album);
    // Albums are folders below the media root; never leave it
    if (dir != root && !dir.startsWith(root + u'/'))
        return {};
    return dir;
}

QVariantMap MediaSource::next()
{
    if (m_files.isEmpty())
        return {};

    if (m_order == u"shuffle") {
        if (m_shuffleQueue.isEmpty())
            refillShuffleQueue();
        m_current = m_shuffleQueue.takeFirst();
    } else {
        const qsizetype index = (m_files.indexOf(m_current) + 1) % m_files.size();
        m_current = m_files.at(index);
    }
    return {
        {QStringLiteral("source"), bestFile(m_current)},
        {QStringLiteral("original"), m_current},
    };
}

// The rendition for our tier, else the other tier's (still far smaller than
// most originals), else the original. Files are checked, in case someone
// cleared the cache.
QString MediaSource::bestFile(const QString &original) const
{
    const QHash<QString, QString> renditions = m_renditions.value(original);
    const QString otherTier = m_tier == u"2160p" ? QStringLiteral("1080p") : QStringLiteral("2160p");
    for (const QString &kind : {m_tier, otherTier}) {
        const QString file = renditions.value(kind);
        if (!file.isEmpty() && QFileInfo::exists(file))
            return file;
    }
    return original;
}

void MediaSource::scheduleRescan(int delayMs)
{
    m_rescanTimer.start(delayMs);
}

void MediaSource::onDirectoryChanged(const QString &path)
{
    // The state directory also holds the display's own file; only a new
    // library generation matters here
    if (path == m_stateDir && !m_libraryState.changed())
        return;
    scheduleRescan(1000);
}

void MediaSource::rescan()
{
    const QString dir = directory();
    watch(dir);

    QStringList files;
    QHash<QString, QHash<QString, QString>> renditions;
    QList<berry::LibraryEntry> entries;
    const bool fromLibrary = !dir.isEmpty() && !m_database.isEmpty()
        && berry::readLibrary(m_database, m_root, m_cacheRoot, m_album, &entries) && !entries.isEmpty();
    if (fromLibrary) {
        for (const berry::LibraryEntry &entry : std::as_const(entries)) {
            files.append(entry.original);
            if (!entry.renditions.isEmpty())
                renditions.insert(entry.original, entry.renditions);
        }
    } else if (!dir.isEmpty()) {
        // No library yet (admin service not running or still on its first sync)
        files = berry::scanImages(dir);
    }

    m_renditions = renditions;
    if (fromLibrary != m_fromLibrary) {
        m_fromLibrary = fromLibrary;
        qInfo().noquote() << "Bilder aus" << (fromLibrary ? "der Mediathek:" : "dem Ordner:") << dir;
        emit fromLibraryChanged();
    }
    if (files == m_files)
        return;

    const int oldCount = count();
    m_files = files;
    m_shuffleQueue.clear(); // new round with the new set of files
    if (count() != oldCount)
        emit countChanged();
}

void MediaSource::watch(const QString &directory)
{
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());

    QStringList dirs;
    if (!m_stateDir.isEmpty() && QFileInfo(m_stateDir).isDir())
        dirs.append(m_stateDir);
    if (!directory.isEmpty() && QFileInfo(directory).isDir()) {
        dirs.append(directory);
        QDirIterator it(directory, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext())
            dirs.append(it.next());
    }
    if (!dirs.isEmpty())
        m_watcher.addPaths(dirs);
}

void MediaSource::refillShuffleQueue()
{
    m_shuffleQueue = m_files;
    std::shuffle(m_shuffleQueue.begin(), m_shuffleQueue.end(), *QRandomGenerator::global());
    // Do not show the same picture twice in a row across rounds
    if (m_shuffleQueue.size() > 1 && m_shuffleQueue.first() == m_current)
        m_shuffleQueue.swapItemsAt(0, m_shuffleQueue.size() - 1);
}
