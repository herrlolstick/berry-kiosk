#pragma once

#include "config.h"
#include "deviceinfo.h"
#include "rendertier.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QSize>
#include <QTimer>
#include <QVariantMap>
#include <QtQmlIntegration>

// Bridge between the configuration and QML: exposes the active layout and the
// render decision, and reloads config.json when it changes on disk.
class KioskController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("KioskController is provided by the application")

    Q_PROPERTY(QVariantMap layout READ layout NOTIFY configChanged)
    Q_PROPERTY(QString mediaRoot READ mediaRoot NOTIFY configChanged)
    Q_PROPERTY(int rotation READ rotation NOTIFY configChanged)
    Q_PROPERTY(QString renderTier READ renderTier NOTIFY renderChanged)
    Q_PROPERTY(QSize renderSize READ renderSize NOTIFY renderChanged)
    Q_PROPERTY(QString renderReason READ renderReason NOTIFY renderChanged)
    Q_PROPERTY(QSize screenSize READ screenSize NOTIFY renderChanged)
    Q_PROPERTY(int maxTextureSize READ maxTextureSize CONSTANT)
    Q_PROPERTY(QString deviceModel READ deviceModel CONSTANT)
    Q_PROPERTY(bool debug READ debug CONSTANT)

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
    };

    explicit KioskController(const Options &options, QObject *parent = nullptr);

    QVariantMap layout() const { return m_layout; }
    QString mediaRoot() const { return m_config.mediaRoot(); }
    int rotation() const { return m_config.display().rotation; }
    QString renderTier() const { return berry::tierName(m_render.tier); }
    QSize renderSize() const { return m_render.renderSize; }
    QString renderReason() const { return m_render.reason; }
    QSize screenSize() const { return m_screenSize; }
    int maxTextureSize() const { return m_options.maxTextureSize; }
    QString deviceModel() const;
    bool debug() const { return m_options.debug; }

signals:
    void configChanged();
    void renderChanged();

private:
    void reloadConfig();
    void updateRender();
    void watchConfigFile();

    Options m_options;
    berry::Config m_config;
    bool m_configLoaded = false;
    QVariantMap m_layout;
    berry::RenderDecision m_render;
    bool m_renderDecided = false;
    QSize m_screenSize;
    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
};
