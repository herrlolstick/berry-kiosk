#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration>

// The images of one album (a folder below the media root) in playing order.
class MediaSource : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString root READ root WRITE setRoot NOTIFY rootChanged)
    Q_PROPERTY(QString album READ album WRITE setAlbum NOTIFY albumChanged)
    Q_PROPERTY(QString order READ order WRITE setOrder NOTIFY orderChanged)
    Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit MediaSource(QObject *parent = nullptr);

    QString root() const { return m_root; }
    void setRoot(const QString &root);
    QString album() const { return m_album; }
    void setAlbum(const QString &album);
    QString order() const { return m_order; } // "sequential" or "shuffle"
    void setOrder(const QString &order);
    QString directory() const;
    int count() const { return int(m_files.size()); }

    // Absolute path of the next image, empty when the album has none
    Q_INVOKABLE QString next();

signals:
    void rootChanged();
    void albumChanged();
    void orderChanged();
    void directoryChanged();
    void countChanged();

private:
    void scheduleRescan(int delayMs);
    void rescan();
    void watch(const QString &directory);
    void refillShuffleQueue();

    QString m_root;
    QString m_album;
    QString m_order = QStringLiteral("sequential");
    QStringList m_files;
    QStringList m_shuffleQueue;
    QString m_current;
    QFileSystemWatcher m_watcher;
    QTimer m_rescanTimer;
    QTimer m_periodicRescan;
};
