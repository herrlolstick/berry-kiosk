#pragma once

#include "config.h"
#include "deviceinfo.h"
#include "rendertier.h"
#include "schedule.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QVariantMap>

#include <optional>
#include <QtQmlIntegration>

// Bridge between the configuration and QML: exposes the active layout and the
// render decision, and reloads config.json when it changes on disk.
class KioskController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("KioskController is provided by the application")

    Q_PROPERTY(QVariantMap layout READ layout NOTIFY layoutChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY layoutChanged)
    Q_PROPERTY(QString mediaRoot READ mediaRoot NOTIFY configChanged)
    Q_PROPERTY(QString libraryDatabase READ libraryDatabase NOTIFY configChanged)
    Q_PROPERTY(QString cacheRoot READ cacheRoot NOTIFY configChanged)
    Q_PROPERTY(QString stateDir READ stateDir NOTIFY configChanged)
    Q_PROPERTY(int rotation READ rotation NOTIFY configChanged)
    Q_PROPERTY(QString locale READ locale NOTIFY configChanged)
    Q_PROPERTY(QVariantList quotes READ quotes NOTIFY configChanged)
    Q_PROPERTY(QString renderTier READ renderTier NOTIFY renderChanged)
    Q_PROPERTY(QSize renderSize READ renderSize NOTIFY renderChanged)
    Q_PROPERTY(QString renderReason READ renderReason NOTIFY renderChanged)
    Q_PROPERTY(QSize screenSize READ screenSize NOTIFY renderChanged)
    Q_PROPERTY(int maxTextureSize READ maxTextureSize CONSTANT)
    Q_PROPERTY(QString deviceModel READ deviceModel CONSTANT)
    Q_PROPERTY(bool debug READ debug CONSTANT)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool screenOff READ screenOff NOTIFY screenOffChanged)
    Q_PROPERTY(bool publicMode READ publicMode CONSTANT)

public:
    struct Options
    {
        QString configPath;
        berry::DeviceInfo device;
        int maxTextureSize = 0; // 0 = unknown
        bool debug = false;
        // With eglfs the KMS mode is fixed at startup, so a new maxRender
        // needs a restart (systemd brings the display back up).
        bool restartOnModeChange = false;
        // Profile "public": no keyboard shortcuts at all (read once at start,
        // eglfs input is switched off before the platform starts)
        bool publicMode = false;
    };

    explicit KioskController(const Options &options, QObject *parent = nullptr);

    QVariantMap layout() const { return m_layout; }
    QString mediaRoot() const { return m_config.mediaRoot(); }
    QString libraryDatabase() const { return m_config.library().database; }
    QString cacheRoot() const { return m_config.library().cacheRoot; }
    QString stateDir() const { return m_config.library().stateDir; }
    int rotation() const { return m_config.display().rotation; }
    QString locale() const { return m_config.locale(); }
    QVariantList quotes() const { return m_config.quotes().toVariantList(); }
    QString renderTier() const { return berry::tierName(m_render.tier); }
    QSize renderSize() const { return m_render.renderSize; }
    QString renderReason() const { return m_render.reason; }
    QSize screenSize() const { return m_screenSize; }
    int maxTextureSize() const { return m_options.maxTextureSize; }
    QString deviceModel() const;
    bool debug() const { return m_options.debug; }
    bool paused() const { return m_paused; }
    void setPaused(bool paused);
    QString layoutId() const { return m_layout.value(QStringLiteral("id")).toString(); }
    bool previewing() const { return !m_previewLayout.isEmpty(); }

    // Shows a layout for a while without saving it (layout editor's preview)
    void previewLayout(const QVariantMap &layout, int seconds);
    void endPreview();
    // Layout the schedule asks for; empty = the configured active layout
    void setScheduledLayout(const QString &id);

    bool screenOff() const { return m_screenOff; }
    bool publicMode() const { return m_options.publicMode; }
    // Switches the screen by hand until the schedule next changes its mind
    void setScreenOn(bool on);

    // Remote control from the admin service
    void requestNext() { emit nextRequested(); }
    // Called by slideshows when a picture starts to show
    Q_INVOKABLE void reportShown(const QString &region, const QString &path) { emit imageShown(region, path); }

signals:
    void configChanged();
    void layoutChanged();
    void renderChanged();
    void pausedChanged();
    void screenOffChanged();
    void nextRequested();
    void imageShown(const QString &region, const QString &path);

private:
    void reloadConfig();
    void updateRender();
    void watchConfigFile();
    void writeDisplayState();
    // Preview before schedule before the configured active layout
    void updateLayout();
    void evaluateSchedule();
    void setScreenOff(bool off);
    void switchScreenPower(bool on);

    Options m_options;
    berry::Config m_config;
    bool m_configLoaded = false;
    QVariantMap m_layout;
    QVariantMap m_previewLayout;
    QTimer m_previewTimer;
    QString m_scheduledLayout;
    QList<berry::ScheduleRule> m_schedule;
    berry::ScheduleState m_scheduleState;
    std::optional<bool> m_manualScreenOff;
    bool m_screenOff = false;
    QTimer m_scheduleTimer;
    berry::RenderDecision m_render;
    bool m_renderDecided = false;
    bool m_paused = false;
    QSize m_screenSize;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
};
