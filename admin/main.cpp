#include "changewatcher.h"
#include "imagemetadata.h"
#include "libraryworker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

#include <memory>

namespace {

constexpr auto kDefaultConfigPath = "/var/lib/berry-kiosk/config.json";

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("berry-kiosk-admin"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BERRY_KIOSK_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("berry-kiosk: Mediathek und Bildvarianten"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption configOption(
        QStringList{QStringLiteral("c"), QStringLiteral("config")}, QStringLiteral("Pfad zur config.json"),
        QStringLiteral("datei"), qEnvironmentVariable("BERRY_KIOSK_CONFIG", QString::fromLatin1(kDefaultConfigPath)));
    const QCommandLineOption onceOption(QStringLiteral("once"),
                                        QStringLiteral("Einmal abgleichen, alle Varianten erstellen und beenden"));
    parser.addOptions({configOption, onceOption});
    parser.process(app);

    const QString configPath = QFileInfo(parser.value(configOption)).absoluteFilePath();
    const bool once = parser.isSet(onceOption);
    qInfo().noquote() << "berry-kiosk-admin" << BERRY_KIOSK_VERSION << "| Konfiguration:" << configPath;

    berry::initializeMetadataReading();

    // Syncing and rendering may take minutes; the main thread stays free for
    // watching files now and for the web API later.
    QThread thread;
    thread.setObjectName(QStringLiteral("library"));
    auto *worker = new LibraryWorker(configPath);
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::started, worker, &LibraryWorker::start);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(worker, &LibraryWorker::fatal, &app, [] { QCoreApplication::exit(1); });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &thread, [&thread] {
        thread.quit();
        thread.wait(); // lets the current rendition finish
    });

    std::unique_ptr<ChangeWatcher> watcher;
    if (once) {
        QObject::connect(worker, &LibraryWorker::idle, &app, &QCoreApplication::quit);
    } else {
        watcher = std::make_unique<ChangeWatcher>(configPath);
        QObject::connect(watcher.get(), &ChangeWatcher::changed, worker, &LibraryWorker::requestSync);
    }

    thread.start();
    return QCoreApplication::exec();
}
