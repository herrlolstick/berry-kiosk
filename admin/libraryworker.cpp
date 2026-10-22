#include "libraryworker.h"

#include "state.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

namespace {

// Often enough for the display to switch to new renditions soon, rarely
// enough not to make it rescan after every single file
constexpr int kStateWriteIntervalMs = 5000;
constexpr int kProgressLogIntervalMs = 30000;

} // namespace

LibraryWorker::LibraryWorker(const QString &configPath, QObject *parent)
    : QObject(parent)
    , m_configPath(configPath)
{
}

LibraryWorker::~LibraryWorker() = default;

void LibraryWorker::start()
{
    schedule();
}

void LibraryWorker::requestSync()
{
    m_syncRequested = true;
    schedule();
}

void LibraryWorker::schedule()
{
    if (m_scheduled)
        return;
    m_scheduled = true;
    // Through the event loop, so sync requests get in between two renditions
    QTimer::singleShot(0, this, &LibraryWorker::step);
}

void LibraryWorker::step()
{
    m_scheduled = false;

    if (m_syncRequested) {
        m_syncRequested = false;
        if (!loadConfig())
            return;

        QElapsedTimer timer;
        timer.start();
        const berry::SyncResult result = m_library->sync();
        m_kinds = wantedKinds();
        if (result.added || result.updated || result.removed || !m_idle) {
            qInfo().noquote() << QStringLiteral("Abgleich: %1 neu, %2 geändert, %3 entfernt, %4 unverändert (%5 ms) "
                                                "– Varianten: %6")
                                     .arg(result.added)
                                     .arg(result.updated)
                                     .arg(result.removed)
                                     .arg(result.unchanged)
                                     .arg(timer.elapsed())
                                     .arg(m_kinds.join(QStringLiteral(", ")));
        }
        m_idle = false;
        writeState(true);
        schedule();
        return;
    }

    if (!m_library)
        return;

    const QList<berry::RenditionJob> jobs = m_library->pendingRenditions(m_kinds, 1);
    if (jobs.isEmpty()) {
        if (!m_idle) {
            qInfo().noquote() << QStringLiteral("Mediathek aktuell: %1 Bilder, %2 Varianten erstellt, %3 fehlgeschlagen")
                                     .arg(m_library->imageCount())
                                     .arg(m_done)
                                     .arg(m_failed);
            m_idle = true;
            writeState(true);
        }
        emit idle();
        return;
    }

    const berry::RenditionJob &job = jobs.constFirst();
    QString error;
    if (m_library->generate(job, m_config.library().jpegQuality, &error)) {
        ++m_done;
    } else {
        ++m_failed;
        qWarning().noquote() << "Variante" << job.kind << "nicht erstellt:" << job.source << '-' << error;
    }

    if (!m_sinceProgressLog.isValid() || m_sinceProgressLog.elapsed() > kProgressLogIntervalMs) {
        qInfo().noquote() << QStringLiteral("Varianten: %1 erstellt, %2 offen").arg(m_done).arg(m_library->pendingCount(m_kinds));
        m_sinceProgressLog.start();
    }
    writeState(false);
    schedule();
}

bool LibraryWorker::loadConfig()
{
    QString error;
    m_config = berry::Config::load(m_configPath, &error);
    if (!error.isEmpty())
        qWarning().noquote() << "Konfiguration:" << error;

    const berry::LibrarySettings settings = m_config.library();
    const QString key = settings.database + u'\n' + m_config.mediaRoot() + u'\n' + settings.cacheRoot;
    if (m_library && key == m_libraryKey)
        return true;

    m_library = std::make_unique<berry::Library>(settings.database, m_config.mediaRoot(), settings.cacheRoot);
    QString openError;
    if (!m_library->open(&openError)) {
        qCritical().noquote() << "Mediathek nicht nutzbar:" << settings.database << '-' << openError;
        m_library.reset();
        m_libraryKey.clear();
        emit fatal(openError);
        return false;
    }
    m_libraryKey = key;
    qInfo().noquote() << "Mediathek:" << settings.database << "| Bilder:" << m_config.mediaRoot()
                      << "| Varianten:" << settings.cacheRoot;
    return true;
}

QStringList LibraryWorker::wantedKinds() const
{
    QStringList kinds = m_config.library().renditions;
    if (kinds.isEmpty()) {
        // Prepare what the display actually uses; before it has said so, 1080p
        const QString tier = berry::readJsonFile(berry::displayStatePath(m_config.library().stateDir))
                                 .value(u"renderTier")
                                 .toString();
        kinds.append(tier == u"2160p" ? tier : QStringLiteral("1080p"));
    }
    kinds.append(QStringLiteral("thumb"));
    return kinds;
}

void LibraryWorker::writeState(bool force)
{
    if (!m_library)
        return;
    if (!force && m_sinceStateWrite.isValid() && m_sinceStateWrite.elapsed() < kStateWriteIntervalMs)
        return;

    const QJsonObject state{
        {QStringLiteral("generation"), ++m_generation},
        {QStringLiteral("images"), m_library->imageCount()},
        {QStringLiteral("pendingRenditions"), m_library->pendingCount(m_kinds)},
        {QStringLiteral("renditions"), QJsonArray::fromStringList(m_kinds)},
        {QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
    };
    QString error;
    if (!berry::writeJsonFile(berry::libraryStatePath(m_config.library().stateDir), state, &error))
        qWarning().noquote() << "Status nicht schreibbar:" << error;
    m_sinceStateWrite.start();
}
