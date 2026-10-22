#pragma once

#include <QCborMap>
#include <QHash>
#include <QList>
#include <QLocalServer>
#include <QObject>
#include <QPointer>

class KioskController;
class QLocalSocket;
class QQuickWindow;

// Lets the admin service control the display over a local socket (see
// core/displayprotocol.h): next picture, pause, screenshot, status. Only the
// same system user can connect.
class DisplayControl : public QObject
{
    Q_OBJECT

public:
    explicit DisplayControl(KioskController *kiosk, QObject *parent = nullptr);

    void setWindow(QQuickWindow *window);
    // Listens in the kiosk's state directory and follows it when it moves
    void start();

private:
    void listen();
    void onNewConnection();
    void onReadyRead(QLocalSocket *client);
    QCborMap handle(const QCborMap &request);
    QCborMap screenshot(const QCborMap &request);
    void broadcast(const QCborMap &event);

    KioskController *m_kiosk;
    QPointer<QQuickWindow> m_window;
    QLocalServer m_server;
    QString m_socketName;
    QList<QLocalSocket *> m_clients;
    QHash<QString, QString> m_shown; // region -> original path
};
