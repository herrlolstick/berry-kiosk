#pragma once

#include "config.h"
#include "library.h"

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>

#include <memory>

// Runs in its own thread: syncs the library with the media folder and then
// works through the missing renditions one at a time, newest pictures first.
// A sync request interrupts the queue between two renditions.
class LibraryWorker : public QObject
{
    Q_OBJECT

public:
    explicit LibraryWorker(const QString &configPath, QObject *parent = nullptr);
    ~LibraryWorker() override;

public slots:
    void start();
    void requestSync();

signals:
    // Library in sync and every rendition done (or failed)
    void idle();
    void fatal(const QString &message);

private:
    void schedule();
    void step();
    bool loadConfig();
    QStringList wantedKinds() const;
    void writeState(bool force);

    QString m_configPath;
    berry::Config m_config;
    std::unique_ptr<berry::Library> m_library;
    QString m_libraryKey;
    QStringList m_kinds;
    bool m_syncRequested = true;
    bool m_scheduled = false;
    bool m_idle = false;
    int m_generation = 0;
    int m_done = 0;
    int m_failed = 0;
    QElapsedTimer m_sinceStateWrite;
    QElapsedTimer m_sinceProgressLog;
};
