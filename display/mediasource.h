#pragma once

#include "state.h"

#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration>

// The images of one album (a folder below the media root) in playing order.
// Reads the media library when the admin service keeps one, otherwise the
// folder itself, and picks the best rendition for the render tier per image.
class MediaSource : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString root READ root WRITE setRoot NOTIFY rootChanged)
    Q_PROPERTY(QString album READ album WRITE setAlbum NOTIFY albumChanged)
    Q_PROPERTY(QString order READ order WRITE setOrder NOTIFY orderChanged)
    Q_PROPERTY(QString database READ database WRITE setDatabase NOTIFY databaseChanged)
    Q_PROPERTY(QString cacheRoot READ cacheRoot WRITE setCacheRoot NOTIFY cacheRootChanged)
    Q_PROPERTY(QString stateDir READ stateDir WRITE setStateDir NOTIFY stateDirChanged)
    Q_PROPERTY(QString tier READ tier WRITE setTier NOTIFY tierChanged)
    Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool fromLibrary READ fromLibrary NOTIFY fromLibraryChanged)

public:
    explicit MediaSource(QObject *parent = nullptr);

    QString root() const { return m_root; }
    void setRoot(const QString &root);
    QString album() const { return m_album; }
    void setAlbum(const QString &album);
    QString order() const { return m_order; } // "sequential" or "shuffle"
    void setOrder(const QString &order);
    QString database() const { return m_database; }
    void setDatabase(const QString &database);
    QString cacheRoot() const { return m_cacheRoot; }
    void setCacheRoot(const QString &cacheRoot);
    QString stateDir() const { return m_stateDir; }
    void setStateDir(const QString &stateDir);
    QString tier() const { return m_tier; } // "1080p" or "2160p"
    void setTier(const QString &tier);
    QString directory() const;
    int count() const { return int(m_files.size()); }
    bool fromLibrary() const { return m_fromLibrary; }

    // { source: file to show, original: the picture itself (for its metadata) },
    // empty when the album has no pictures
    Q_INVOKABLE QVariantMap next();

signals:
    void rootChanged();
    void albumChanged();
    void orderChanged();
    void databaseChanged();
    void cacheRootChanged();
    void stateDirChanged();
    void tierChanged();
    void directoryChanged();
    void countChanged();
    void fromLibraryChanged();

private:
    void scheduleRescan(int delayMs);
    void rescan();
    void watch(const QString &directory);
    void onDirectoryChanged(const QString &path);
    void refillShuffleQueue();
    QString bestFile(const QString &original) const;

    QString m_root;
    QString m_album;
    QString m_order = QStringLiteral("sequential");
    QString m_database;
    QString m_cacheRoot;
    QString m_stateDir;
    QString m_tier = QStringLiteral("1080p");
    QStringList m_files; // originals
    QHash<QString, QHash<QString, QString>> m_renditions; // original -> kind -> file
    bool m_fromLibrary = false;
    QStringList m_shuffleQueue;
    QString m_current;
    berry::FileSnapshot m_libraryState;
    QFileSystemWatcher m_watcher;
    QTimer m_rescanTimer;
    QTimer m_periodicRescan;
};
