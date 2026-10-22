#include "displaylink.h"

#include "displayprotocol.h"

#include <QDeadlineTimer>

DisplayLink::DisplayLink(QObject *parent)
    : QObject(parent)
{
    m_reconnect.setSingleShot(true);
    m_reconnect.setInterval(2000);
    connect(&m_reconnect, &QTimer::timeout, this, &DisplayLink::connectNow);

    connect(&m_socket, &QLocalSocket::readyRead, this, &DisplayLink::onReadyRead);
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        qInfo().noquote() << "Anzeige verbunden:" << m_name;
        emit connectedChanged(true);
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        qInfo() << "Anzeige getrennt";
        m_shown.clear();
        emit connectedChanged(false);
    });
    // Covers both a lost connection and a failed attempt (the display is not
    // up yet): errorOccurred() comes before the state is back to unconnected
    connect(&m_socket, &QLocalSocket::stateChanged, this, [this](QLocalSocket::LocalSocketState state) {
        if (state == QLocalSocket::UnconnectedState)
            m_reconnect.start();
    });
}

DisplayLink::~DisplayLink()
{
    // The socket emits disconnected() while being destroyed, after the timer is gone
    m_socket.disconnect(this);
    m_socket.abort();
}

void DisplayLink::setSocketName(const QString &name)
{
    if (m_name == name)
        return;
    m_name = name;
    m_socket.abort();
    connectNow();
}

bool DisplayLink::isConnected() const
{
    return m_socket.state() == QLocalSocket::ConnectedState;
}

void DisplayLink::connectNow()
{
    if (m_name.isEmpty() || m_socket.state() != QLocalSocket::UnconnectedState)
        return;
    m_socket.connectToServer(m_name);
}

QCborMap DisplayLink::request(const QString &command, const QCborMap &arguments, int timeoutMs, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QCborMap();
    };
    if (!isConnected())
        return fail(QStringLiteral("Anzeige nicht verbunden"));

    const qint64 id = ++m_nextId;
    QCborMap message = arguments;
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("command"), command);
    m_waiting.insert(id);
    berry::writeFrame(&m_socket, message);
    m_socket.flush();

    // waitForReadyRead() emits readyRead, so onReadyRead() collects the answer
    const QDeadlineTimer deadline(timeoutMs);
    while (!m_responses.contains(id)) {
        if (deadline.hasExpired() || !isConnected()) {
            m_waiting.remove(id);
            return fail(QStringLiteral("Anzeige antwortet nicht"));
        }
        m_socket.waitForReadyRead(int(qMax<qint64>(1, deadline.remainingTime())));
    }
    m_waiting.remove(id);
    const QCborMap response = m_responses.take(id);
    if (!response.value(QStringLiteral("ok")).toBool())
        return fail(response.value(QStringLiteral("error")).toString());
    return response;
}

void DisplayLink::onReadyRead()
{
    for (const QCborMap &frame : berry::readFrames(&m_socket)) {
        if (frame.contains(QStringLiteral("id"))) {
            const qint64 id = frame.value(QStringLiteral("id")).toInteger();
            if (m_waiting.contains(id)) // late answers to timed out requests are dropped
                m_responses.insert(id, frame);
        } else if (frame.value(QStringLiteral("event")).toString() == u"shown") {
            m_shown.insert(frame.value(QStringLiteral("region")).toString(),
                           frame.value(QStringLiteral("path")).toString());
        }
    }
}
