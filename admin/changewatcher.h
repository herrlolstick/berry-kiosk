#pragma once

#include "state.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

// Watches everything that should trigger a library sync: the media folder
// (recursively), config.json and the display's state file. Changes are
// collected for a moment, so copying a hundred pictures causes one sync.
class ChangeWatcher : public QObject
{
    Q_OBJECT

public:
    explicit ChangeWatcher(const QString &configPath, QObject *parent = nullptr);

signals:
    void changed();

private:
    void rewatch();
    void onDirectoryChanged(const QString &path);

    QString m_configPath;
    QString m_configDir;
    QString m_mediaRoot;
    QString m_stateDir;
    berry::FileSnapshot m_config;
    berry::FileSnapshot m_displayState;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_periodic;
};
