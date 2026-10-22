#include "kioskcontroller.h"

#include "state.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QScreen>
#include <QSysInfo>

KioskController::KioskController(const Options &options, QObject *parent)
    : QObject(parent)
    , m_options(options)
{
    // Editors and the admin server replace the file in several steps; wait for the dust to settle
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(300);
    connect(&m_reloadTimer, &QTimer::timeout, this, &KioskController::reloadConfig);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reloadTimer, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reloadTimer, qOverload<>(&QTimer::start));

    m_previewTimer.setSingleShot(true);
    connect(&m_previewTimer, &QTimer::timeout, this, &KioskController::endPreview);

    if (QScreen *screen = QGuiApplication::primaryScreen())
        connect(screen, &QScreen::geometryChanged, this, &KioskController::updateRender);

    reloadConfig();
}

void KioskController::previewLayout(const QVariantMap &layout, int seconds)
{
    m_previewLayout = layout;
    if (!m_previewLayout.contains(QStringLiteral("id")))
        m_previewLayout.insert(QStringLiteral("id"), QStringLiteral("vorschau"));
    m_previewTimer.start(qBound(1, seconds, 600) * 1000);
    qInfo().noquote() << "Vorschau für" << qBound(1, seconds, 600) << "s:" << m_previewLayout.value(QStringLiteral("id")).toString();
    updateLayout();
}

void KioskController::endPreview()
{
    m_previewTimer.stop();
    if (m_previewLayout.isEmpty())
        return;
    m_previewLayout.clear();
    updateLayout();
}

void KioskController::setScheduledLayout(const QString &id)
{
    if (m_scheduledLayout == id)
        return;
    m_scheduledLayout = id;
    updateLayout();
}

void KioskController::updateLayout()
{
    QVariantMap layout = m_previewLayout;
    if (layout.isEmpty() && !m_scheduledLayout.isEmpty())
        layout = m_config.layoutById(m_scheduledLayout).toVariantMap();
    if (layout.isEmpty())
        layout = m_config.activeLayout().toVariantMap();
    if (layout == m_layout)
        return;
    m_layout = layout;
    emit layoutChanged();
}

void KioskController::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    emit pausedChanged();
}

QString KioskController::deviceModel() const
{
    return m_options.device.model.isEmpty() ? QSysInfo::prettyProductName() : m_options.device.model;
}

void KioskController::reloadConfig()
{
    QString error;
    const berry::Config config = berry::Config::load(m_options.configPath, &error);
    watchConfigFile();

    if (!error.isEmpty()) {
        qWarning().noquote() << "Konfiguration:" << error;
        // Keep showing the last good configuration instead of jumping to defaults mid-show
        if (m_configLoaded)
            return;
    }
    if (m_configLoaded && config == m_config)
        return;

    const bool modeChanged = m_configLoaded && config.display().maxRender != m_config.display().maxRender;
    m_config = config;
    m_configLoaded = true;

    if (modeChanged && m_options.restartOnModeChange) {
        qInfo() << "maxRender geändert, Neustart für den neuen Bildschirmmodus";
        QCoreApplication::exit(0);
        return;
    }

    emit configChanged();
    updateLayout();
    updateRender();
    writeDisplayState(); // the state directory may have moved
}

void KioskController::updateRender()
{
    QSize screen;
    if (QScreen *s = QGuiApplication::primaryScreen())
        screen = (QSizeF(s->size()) * s->devicePixelRatio()).toSize();

    const berry::DisplaySettings display = m_config.display();
    if (display.rotation == 90 || display.rotation == 270)
        screen.transpose();

    const berry::RenderDecision decision =
        berry::decideRenderTier(screen, m_options.maxTextureSize, display.maxRender);
    if (m_renderDecided && screen == m_screenSize && decision.tier == m_render.tier
        && decision.renderSize == m_render.renderSize && decision.reason == m_render.reason) {
        return;
    }

    m_screenSize = screen;
    m_render = decision;
    m_renderDecided = true;
    qInfo().noquote() << QStringLiteral("Render-Stufe %1, Bilder bis %2×%3 (%4)")
                             .arg(berry::tierName(decision.tier))
                             .arg(decision.renderSize.width())
                             .arg(decision.renderSize.height())
                             .arg(decision.reason);
    emit renderChanged();
    writeDisplayState();
}

// Tells the admin service which renditions this display needs
void KioskController::writeDisplayState()
{
    if (!m_renderDecided)
        return;
    const QJsonObject state{
        {QStringLiteral("renderTier"), renderTier()},
        {QStringLiteral("renderSize"), QJsonArray{m_render.renderSize.width(), m_render.renderSize.height()}},
        {QStringLiteral("screenSize"), QJsonArray{m_screenSize.width(), m_screenSize.height()}},
        {QStringLiteral("rotation"), rotation()},
        {QStringLiteral("maxTextureSize"), m_options.maxTextureSize},
        {QStringLiteral("device"), deviceModel()},
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
    };
    const QString path = berry::displayStatePath(stateDir());
    // Unchanged content keeps the file untouched, so the admin is not woken up for nothing
    if (berry::readJsonFile(path) == state)
        return;
    QString error;
    if (!berry::writeJsonFile(path, state, &error))
        qWarning().noquote() << "Anzeigestatus nicht schreibbar:" << path << error;
}

void KioskController::watchConfigFile()
{
    // Atomic saves replace the file, which drops the file watch; the directory
    // watch notices the new file and we pick it up again here.
    const QString path = m_options.configPath;
    const QString dir = QFileInfo(path).absolutePath();
    if (!m_watcher.directories().contains(dir) && QFileInfo::exists(dir))
        m_watcher.addPath(dir);
    if (!m_watcher.files().contains(path) && QFileInfo::exists(path))
        m_watcher.addPath(path);
}
