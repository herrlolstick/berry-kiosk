#include "apiserver.h"
#include "auth.h"
#include "changewatcher.h"
#include "config.h"
#include "displaylink.h"
#include "displayprotocol.h"
#include "imagemetadata.h"
#include "libraryworker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTextStream>
#include <QThread>

#include <cstdio>
#include <memory>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr auto kDefaultConfigPath = "/var/lib/berry-kiosk/config.json";

// Reads a line from the terminal without echo, or plainly from a pipe
QString readSecret(const QString &prompt)
{
    const bool terminal = isatty(STDIN_FILENO);
    QTextStream err(stderr);
    termios saved{};
    if (terminal) {
        err << prompt << Qt::flush;
        tcgetattr(STDIN_FILENO, &saved);
        termios silent = saved;
        silent.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    }
    const QString line = QTextStream(stdin).readLine();
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        err << Qt::endl;
    }
    return line;
}

int setPassword(const QString &configPath, const QString &username)
{
    QTextStream err(stderr);
    const QString password = readSecret(QStringLiteral("Neues Passwort für %1: ").arg(username));
    if (password.size() < Credentials::kMinimumPasswordLength) {
        err << "Das Passwort braucht mindestens " << Credentials::kMinimumPasswordLength << " Zeichen." << Qt::endl;
        return 1;
    }
    if (isatty(STDIN_FILENO) && readSecret(QStringLiteral("Wiederholen: ")) != password) {
        err << "Die Passwörter stimmen nicht überein." << Qt::endl;
        return 1;
    }

    const QString path = credentialsPath(berry::Config::load(configPath).library().stateDir);
    QString error;
    if (!saveCredentials(path, Credentials::create(username, password), &error)) {
        err << "Speichern fehlgeschlagen: " << error << Qt::endl;
        return 1;
    }
    err << "Passwort gespeichert in " << path << ". Bestehende Anmeldungen sind damit beendet." << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("berry-kiosk-admin"));
    QCoreApplication::setApplicationVersion(QStringLiteral(BERRY_KIOSK_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("berry-kiosk: Mediathek, Bildvarianten und Verwaltung"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption configOption(
        QStringList{QStringLiteral("c"), QStringLiteral("config")}, QStringLiteral("Pfad zur config.json"),
        QStringLiteral("datei"), qEnvironmentVariable("BERRY_KIOSK_CONFIG", QString::fromLatin1(kDefaultConfigPath)));
    const QCommandLineOption onceOption(QStringLiteral("once"),
                                        QStringLiteral("Einmal abgleichen, alle Varianten erstellen und beenden"));
    const QCommandLineOption setPasswordOption(QStringLiteral("set-password"),
                                               QStringLiteral("Passwort für die Verwaltung setzen und beenden"));
    const QCommandLineOption userOption(QStringLiteral("user"), QStringLiteral("Benutzername für --set-password"),
                                        QStringLiteral("name"), QStringLiteral("admin"));
    parser.addOptions({configOption, onceOption, setPasswordOption, userOption});
    parser.process(app);

    const QString configPath = QFileInfo(parser.value(configOption)).absoluteFilePath();
    if (parser.isSet(setPasswordOption))
        return setPassword(configPath, parser.value(userOption).trimmed());

    const bool once = parser.isSet(onceOption);
    qInfo().noquote() << "berry-kiosk-admin" << BERRY_KIOSK_VERSION << "| Konfiguration:" << configPath;

    berry::initializeMetadataReading();

    // Syncing and rendering may take minutes; the main thread stays free for
    // watching files and serving the web API.
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
    std::unique_ptr<DisplayLink> display;
    std::unique_ptr<ApiServer> api;
    if (once) {
        QObject::connect(worker, &LibraryWorker::idle, &app, &QCoreApplication::quit);
    } else {
        watcher = std::make_unique<ChangeWatcher>(configPath);
        QObject::connect(watcher.get(), &ChangeWatcher::changed, worker, &LibraryWorker::requestSync);

        display = std::make_unique<DisplayLink>();
        const auto followDisplay = [&display, configPath] {
            display->setSocketName(berry::displaySocketName(berry::Config::load(configPath).library().stateDir));
        };
        followDisplay();
        QObject::connect(watcher.get(), &ChangeWatcher::changed, display.get(), followDisplay);

        const berry::AdminSettings settings = berry::Config::load(configPath).admin();
        api = std::make_unique<ApiServer>(configPath, display.get());
        QObject::connect(api.get(), &ApiServer::syncRequested, worker, &LibraryWorker::requestSync);
        QString error;
        if (!api->listen(QHostAddress(settings.address), settings.port, &error)) {
            qCritical().noquote() << QStringLiteral("Web-API nicht startbar auf %1:%2 – %3")
                                         .arg(settings.address)
                                         .arg(settings.port)
                                         .arg(error);
            return 1;
        }
        qInfo().noquote() << QStringLiteral("Verwaltung: http://%1:%2/").arg(settings.address).arg(api->serverPort());
        if (!loadCredentials(credentialsPath(berry::Config::load(configPath).library().stateDir)).isValid())
            qWarning().noquote() << "Noch kein Passwort gesetzt: berry-kiosk-admin --config" << configPath << "--set-password";
    }

    thread.start();
    return QCoreApplication::exec();
}
