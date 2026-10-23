#include "changewatcher.h"

#include "config.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

ChangeWatcher::ChangeWatcher(const QString &configPath, QObject *parent)
    : QObject(parent)
    , m_configPath(configPath)
    , m_configDir(QFileInfo(configPath).absolutePath())
    , m_config(configPath)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(2000);
    connect(&m_debounce, &QTimer::timeout, this, &ChangeWatcher::changed);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &ChangeWatcher::onDirectoryChanged);

    // Network shares do not always report changes
    m_periodic.setInterval(10 * 60 * 1000);
    connect(&m_periodic, &QTimer::timeout, this, &ChangeWatcher::changed);
    m_periodic.start();

    rewatch();
}

void ChangeWatcher::onDirectoryChanged(const QString &path)
{
    if (path == m_configDir) {
        // Also fires for the database's journal files; only react to config.json
        if (m_config.changed()) {
            rewatch();
            m_debounce.start();
        }
    } else if (path == m_stateDir) {
        // Ignore our own library.json, react when the display's tier changes
        if (m_displayState.changed())
            m_debounce.start();
    } else {
        rewatch(); // new sub folders need watching too
        m_debounce.start();
    }
}

void ChangeWatcher::rewatch()
{
    const berry::Config config = berry::Config::load(m_configPath);
    m_mediaRoot = config.mediaRoot();
    m_stateDir = config.library().stateDir;
    QDir().mkpath(m_stateDir);
    if (m_displayState.path() != berry::displayStatePath(m_stateDir))
        m_displayState.setPath(berry::displayStatePath(m_stateDir));

    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());

    QStringList dirs{m_configDir, m_stateDir};
    if (QFileInfo(m_mediaRoot).isDir()) {
        dirs.append(m_mediaRoot);
        QDirIterator it(m_mediaRoot, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString dir = it.next();
            if (!it.fileName().startsWith(u'.') && !it.fileName().startsWith(u'@'))
                dirs.append(dir);
        }
    }
    dirs.removeDuplicates();
    m_watcher.addPaths(dirs);
}
