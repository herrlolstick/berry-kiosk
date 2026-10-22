#include "displaycontrol.h"

#include "displayprotocol.h"
#include "kioskcontroller.h"

#include <QBuffer>
#include <QCborArray>
#include <QDir>
#include <QImage>
#include <QLocalSocket>
#include <QQuickWindow>

DisplayControl::DisplayControl(KioskController *kiosk, QObject *parent)
    : QObject(parent)
    , m_kiosk(kiosk)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &DisplayControl::onNewConnection);
    connect(m_kiosk, &KioskController::configChanged, this, &DisplayControl::listen);
    connect(m_kiosk, &KioskController::imageShown, this, [this](const QString &region, const QString &path) {
        m_shown.insert(region, path);
        broadcast({{QStringLiteral("event"), QStringLiteral("shown")},
                   {QStringLiteral("region"), region},
                   {QStringLiteral("path"), path}});
    });
    connect(m_kiosk, &KioskController::layoutChanged, this, [this] { m_shown.clear(); });
}

void DisplayControl::setWindow(QQuickWindow *window)
{
    m_window = window;
}

void DisplayControl::start()
{
    listen();
}

void DisplayControl::listen()
{
    const QString name = berry::displaySocketName(m_kiosk->stateDir());
    if (name == m_socketName && m_server.isListening())
        return;

    m_server.close();
    QDir().mkpath(m_kiosk->stateDir());
    // A crashed display leaves its socket file behind
    QLocalServer::removeServer(name);
    if (!m_server.listen(name)) {
        qWarning().noquote() << "Steuer-Socket nicht verfügbar:" << name << m_server.errorString();
        return;
    }
    m_socketName = name;
    qInfo().noquote() << "Steuer-Socket:" << m_server.fullServerName();
}

void DisplayControl::onNewConnection()
{
    while (QLocalSocket *client = m_server.nextPendingConnection()) {
        m_clients.append(client);
        connect(client, &QLocalSocket::readyRead, this, [this, client] { onReadyRead(client); });
        connect(client, &QLocalSocket::disconnected, this, [this, client] {
            m_clients.removeAll(client);
            client->deleteLater();
        });
    }
}

void DisplayControl::onReadyRead(QLocalSocket *client)
{
    for (const QCborMap &request : berry::readFrames(client)) {
        QCborMap response = handle(request);
        response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
        berry::writeFrame(client, response);
    }
}

QCborMap DisplayControl::handle(const QCborMap &request)
{
    const QString command = request.value(QStringLiteral("command")).toString();
    const QCborMap ok{{QStringLiteral("ok"), true}};

    if (command == u"status") {
        QCborMap shown;
        for (auto it = m_shown.cbegin(); it != m_shown.cend(); ++it)
            shown.insert(it.key(), it.value());
        return {
            {QStringLiteral("ok"), true},
            {QStringLiteral("paused"), m_kiosk->paused()},
            {QStringLiteral("layout"), m_kiosk->layoutId()},
            {QStringLiteral("previewing"), m_kiosk->previewing()},
            {QStringLiteral("screenOff"), m_kiosk->screenOff()},
            {QStringLiteral("renderTier"), m_kiosk->renderTier()},
            {QStringLiteral("shown"), shown},
        };
    }
    if (command == u"next") {
        m_kiosk->requestNext();
        return ok;
    }
    if (command == u"pause" || command == u"resume") {
        m_kiosk->setPaused(command == u"pause");
        return ok;
    }
    if (command == u"screenshot")
        return screenshot(request);
    if (command == u"preview") {
        const QCborMap layout = request.value(QStringLiteral("layout")).toMap();
        if (layout.isEmpty())
            return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("kein Layout")}};
        m_kiosk->previewLayout(layout.toVariantMap(), int(request.value(QStringLiteral("seconds")).toInteger(60)));
        return ok;
    }
    if (command == u"screen") {
        m_kiosk->setScreenOn(request.value(QStringLiteral("on")).toBool());
        return ok;
    }
    if (command == u"endPreview") {
        m_kiosk->endPreview();
        return ok;
    }

    return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("unbekannter Befehl: %1").arg(command)}};
}

QCborMap DisplayControl::screenshot(const QCborMap &request)
{
    if (!m_window)
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("kein Fenster")}};

    QImage image = m_window->grabWindow();
    if (image.isNull())
        return {{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("Bildschirmfoto fehlgeschlagen")}};

    const qint64 requested = request.value(QStringLiteral("maxWidth")).toInteger(1280);
    const int maxWidth = int(qBound(qint64(160), requested, qint64(3840)));
    if (image.width() > maxWidth)
        image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 80);
    return {
        {QStringLiteral("ok"), true},
        {QStringLiteral("image"), jpeg},
        {QStringLiteral("width"), image.width()},
        {QStringLiteral("height"), image.height()},
    };
}

void DisplayControl::broadcast(const QCborMap &event)
{
    for (QLocalSocket *client : std::as_const(m_clients))
        berry::writeFrame(client, event);
}
