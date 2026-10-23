#pragma once

#include "auth.h"
#include "config.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QHttpServer>
#include <QObject>

#include <optional>

class DisplayLink;
class QHttpServerRequest;
class QHttpServerResponse;
class QTcpServer;

// The REST API under /api and the bundled web UI.
//
// Every route except login and session needs a valid session cookie. Requests
// that change something also need the header "X-Berry-Kiosk: 1": browsers
// only send custom headers same-origin, which together with SameSite=Strict
// cookies shuts out cross-site requests (CSRF).
//
// Changes go to the media folder (uploads, sidecars, trash) and config.json;
// the library worker picks them up via syncRequested().
class ApiServer : public QObject
{
    Q_OBJECT

public:
    ApiServer(const QString &configPath, DisplayLink *display, QObject *parent = nullptr);
    ~ApiServer() override;

    bool listen(const QHostAddress &address, quint16 port, QString *error = nullptr);
    quint16 serverPort() const;

signals:
    void syncRequested();

private:
    void addRoutes();
    berry::Config config() const;
    Credentials credentials() const;
    // An error response when the request may not pass, otherwise nothing
    std::optional<QHttpServerResponse> checkAccess(const QHttpServerRequest &request, bool changes);
    // Host name check, plus CSRF header and Origin for requests that change something
    std::optional<QHttpServerResponse> checkOrigin(const QHttpServerRequest &request, bool changes) const;
    bool isAllowedHost(const QHttpServerRequest &request) const;

    QHttpServerResponse login(const QHttpServerRequest &request);
    QHttpServerResponse logout(const QHttpServerRequest &request);
    QHttpServerResponse session(const QHttpServerRequest &request);
    QHttpServerResponse status();
    QHttpServerResponse listMedia(const QHttpServerRequest &request);
    QHttpServerResponse getMedia(qint64 id);
    QHttpServerResponse updateMedia(qint64 id, const QHttpServerRequest &request);
    QHttpServerResponse deleteMedia(qint64 id);
    QHttpServerResponse mediaFile(qint64 id, const QString &kind);
    QHttpServerResponse batchMedia(const QHttpServerRequest &request);
    QHttpServerResponse upload(const QHttpServerRequest &request);
    QHttpServerResponse renameAlbum(const QHttpServerRequest &request);
    QHttpServerResponse deleteAlbum(const QHttpServerRequest &request);
    QHttpServerResponse changePassword(const QHttpServerRequest &request);
    QHttpServerResponse listAlbums();
    QHttpServerResponse createAlbum(const QHttpServerRequest &request);
    QHttpServerResponse getConfig();
    QHttpServerResponse putConfig(const QHttpServerRequest &request);
    QHttpServerResponse displayCommand(const QString &command);
    QHttpServerResponse screenshot(const QHttpServerRequest &request);
    QHttpServerResponse previewLayout(const QHttpServerRequest &request);

    QString m_configPath;
    DisplayLink *m_display;
    QHttpServer m_http;
    QTcpServer *m_tcp = nullptr;
    SessionStore m_sessions;
    LoginThrottle m_throttle;
    QElapsedTimer m_uptime;
};
