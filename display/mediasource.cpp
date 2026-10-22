#include "mediasource.h"

#include "imagescanner.h"

#include <QDir>
#include <QDirIterator>
#include <QRandomGenerator>

#include <algorithm>

MediaSource::MediaSource(QObject *parent)
    : QObject(parent)
{
    m_rescanTimer.setSingleShot(true);
    connect(&m_rescanTimer, &QTimer::timeout, this, &MediaSource::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { scheduleRescan(1000); });

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

QString MediaSource::next()
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
    return m_current;
}

void MediaSource::scheduleRescan(int delayMs)
{
    m_rescanTimer.start(delayMs);
}

void MediaSource::rescan()
{
    const QString dir = directory();
    watch(dir);

    const QStringList files = dir.isEmpty() ? QStringList() : berry::scanImages(dir);
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
    if (directory.isEmpty() || !QDir(directory).exists())
        return;

    QStringList dirs{directory};
    QDirIterator it(directory, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        dirs.append(it.next());
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
