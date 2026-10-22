#pragma once

#include <QCborMap>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QSet>
#include <QTimer>

// The admin side of the display socket (see core/displayprotocol.h).
// Reconnects by itself whenever the display restarts.
class DisplayLink : public QObject
{
    Q_OBJECT

public:
    explicit DisplayLink(QObject *parent = nullptr);
    ~DisplayLink() override;

    void setSocketName(const QString &name);
    bool isConnected() const;

    // Sends a command and waits for the answer. Blocks the calling thread for
    // at most timeoutMs, which the few HTTP requests that need the display
    // can afford. Returns an empty map and sets error on failure.
    QCborMap request(const QString &command, const QCborMap &arguments = {}, int timeoutMs = 3000,
                     QString *error = nullptr);

    // Last picture per region, from the display's "shown" events
    QHash<QString, QString> shown() const { return m_shown; }

signals:
    void connectedChanged(bool connected);

private:
    void connectNow();
    void onReadyRead();

    QString m_name;
    QLocalSocket m_socket;
    QTimer m_reconnect;
    qint64 m_nextId = 0;
    QSet<qint64> m_waiting;
    QHash<qint64, QCborMap> m_responses;
    QHash<QString, QString> m_shown;
};
