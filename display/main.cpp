#include "config.h"
#include "deviceinfo.h"
#include "kioskcontroller.h"
#include "mediaimageprovider.h"

#include <QCommandLineParser>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

namespace {

constexpr auto kDefaultConfigPath = "/var/lib/berry-kiosk/config.json";

// Looks up "--name value" / "--name=value" before QGuiApplication exists.
QString earlyArgument(int argc, char *argv[], const QStringList &names)
{
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        for (const QString &name : names) {
            if (arg == name && i + 1 < argc)
                return QString::fromLocal8Bit(argv[i + 1]);
            if (arg.startsWith(name + u'='))
                return arg.mid(name.size() + 1);
        }
    }
    return {};
}

bool usesEglfs(int argc, char *argv[])
{
    QString platform = earlyArgument(argc, argv, {QStringLiteral("-platform"), QStringLiteral("--platform")});
    if (platform.isEmpty())
        platform = qEnvironmentVariable("QT_QPA_PLATFORM");
    return platform.startsWith(u"eglfs");
}

// eglfs drives the screen with its preferred mode. On VideoCore IV boards and
// when 4K is switched off we ask for 1080p instead, so a 4K TV runs at a mode
// the GPU can fill. Screens without a 1080p mode keep their preferred one.
// Has to run before QGuiApplication: eglfs reads this file during startup.
void prepareKmsConfig(const berry::Config &config, const berry::DeviceInfo &device)
{
    if (qEnvironmentVariableIsSet("QT_QPA_EGLFS_KMS_CONFIG"))
        return; // set by hand, that wins
    if (!device.legacyGpu && config.display().maxRender != berry::MaxRender::Hd1080)
        return;

    QJsonArray outputs;
    for (const char *name : {"HDMI1", "HDMI2"})
        outputs.append(QJsonObject{{QStringLiteral("name"), QLatin1String(name)},
                                   {QStringLiteral("mode"), QStringLiteral("1920x1080")}});

    // systemd's RuntimeDirectory= sets RUNTIME_DIRECTORY
    const QString dir = qEnvironmentVariable("RUNTIME_DIRECTORY", QDir::tempPath());
    const QString path = dir + QStringLiteral("/eglfs-kms.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << "KMS-Konfiguration nicht schreibbar:" << path << file.errorString();
        return;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("outputs"), outputs}}).toJson());
    file.close();
    qputenv("QT_QPA_EGLFS_KMS_CONFIG", QFile::encodeName(path));
}

int queryMaxTextureSize()
{
    QOpenGLContext context;
    if (!context.create())
        return 0;
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!surface.isValid() || !context.makeCurrent(&surface))
        return 0;

    GLint size = 0;
    context.functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
    context.doneCurrent();
    return size;
}

} // namespace

int main(int argc, char *argv[])
{
    QString configPath = earlyArgument(argc, argv, {QStringLiteral("--config"), QStringLiteral("-c")});
    if (configPath.isEmpty())
        configPath = qEnvironmentVariable("BERRY_KIOSK_CONFIG", QString::fromLatin1(kDefaultConfigPath));
    configPath = QFileInfo(configPath).absoluteFilePath();

    const bool eglfs = usesEglfs(argc, argv);
    const berry::DeviceInfo device = berry::DeviceInfo::detect();
    if (eglfs)
        prepareKmsConfig(berry::Config::load(configPath), device);

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("berry-kiosk-display"));
    QGuiApplication::setApplicationVersion(QStringLiteral(BERRY_KIOSK_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("berry-kiosk: Bilderrahmen und Kiosk-Anzeige"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption configOption(QStringList{QStringLiteral("c"), QStringLiteral("config")},
                                          QStringLiteral("Pfad zur config.json"), QStringLiteral("datei"),
                                          QString::fromLatin1(kDefaultConfigPath));
    const QCommandLineOption windowedOption(QStringList{QStringLiteral("w"), QStringLiteral("windowed")},
                                            QStringLiteral("Im Fenster statt im Vollbild starten (Entwicklung)"));
    const QCommandLineOption debugOption(QStringList{QStringLiteral("d"), QStringLiteral("debug")},
                                         QStringLiteral("Debug-Overlay mit Render-Stufe und FPS anzeigen"));
    const QCommandLineOption snapshotOption(QStringLiteral("snapshot"),
                                            QStringLiteral("Bildschirmfoto nach <sekunden> speichern und beenden (Tests)"),
                                            QStringLiteral("datei"));
    const QCommandLineOption snapshotDelayOption(QStringLiteral("snapshot-delay"),
                                                 QStringLiteral("Wartezeit für --snapshot in Sekunden"),
                                                 QStringLiteral("sekunden"), QStringLiteral("5"));
    parser.addOptions({configOption, windowedOption, debugOption, snapshotOption, snapshotDelayOption});
    parser.process(app);

    KioskController::Options options;
    options.configPath = configPath;
    options.device = device;
    options.maxTextureSize = queryMaxTextureSize();
    if (options.maxTextureSize <= 0 && device.legacyGpu)
        options.maxTextureSize = 2048;
    options.debug = parser.isSet(debugOption);
    options.restartOnModeChange = eglfs;

    qInfo().noquote() << "berry-kiosk" << BERRY_KIOSK_VERSION << "| Konfiguration:" << configPath
                      << "| Gerät:" << (device.model.isEmpty() ? QStringLiteral("unbekannt") : device.model)
                      << "| max. Textur:" << options.maxTextureSize;

    KioskController controller(options);

    const bool windowed = parser.isSet(windowedOption) && !eglfs;
    if (!windowed && !eglfs)
        QGuiApplication::setOverrideCursor(Qt::BlankCursor); // eglfs: QT_QPA_EGLFS_HIDECURSOR

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("media"), new MediaImageProvider(options.maxTextureSize));
    engine.setInitialProperties({
        {QStringLiteral("kiosk"), QVariant::fromValue(&controller)},
        {QStringLiteral("windowed"), windowed},
    });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("BerryKiosk", "Main");

    if (parser.isSet(snapshotOption)) {
        const QString file = parser.value(snapshotOption);
        QTimer::singleShot(qRound(parser.value(snapshotDelayOption).toDouble() * 1000), &app, [&engine, file] {
            auto *window = engine.rootObjects().isEmpty()
                ? nullptr
                : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
            const bool saved = window && window->grabWindow().save(file);
            qInfo().noquote() << (saved ? "Bildschirmfoto gespeichert:" : "Bildschirmfoto fehlgeschlagen:") << file;
            QCoreApplication::exit(saved ? 0 : 1);
        });
    }

    return QGuiApplication::exec();
}
