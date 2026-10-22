#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QString>

#include <optional>

// One administrator account, stored as a salted PBKDF2-SHA256 hash in
// <state>/auth.json (readable by the service user only).
struct Credentials
{
    QString username;
    QString algorithm;
    int iterations = 0;
    QByteArray salt;
    QByteArray hash;

    bool isValid() const;
    // Changes with every new password, so old sessions end
    QByteArray stamp() const { return hash; }

    static Credentials create(const QString &username, const QString &password, int iterations = kDefaultIterations);
    static Credentials fromJson(const QJsonObject &json);
    QJsonObject toJson() const;
    // Takes the same time for a wrong user name as for a wrong password
    bool verify(const QString &username, const QString &password) const;

    // About half a second on a Pi Zero 2 W; stored with the hash, so it can grow later
    static constexpr int kDefaultIterations = 200000;
    static constexpr int kMinimumPasswordLength = 8;
};

QString credentialsPath(const QString &stateDir);
Credentials loadCredentials(const QString &path);
bool saveCredentials(const QString &path, const Credentials &credentials, QString *error = nullptr);

// Logged-in browsers, by random cookie token. Kept in memory: a restart of
// the service logs everybody out, which is fine for a kiosk.
class SessionStore
{
public:
    explicit SessionStore(qint64 lifetimeMs = 12 * 60 * 60 * 1000);

    QString create(const QString &username, const QByteArray &stamp);
    // User name of a valid session; extends it (sliding expiry)
    std::optional<QString> validate(const QString &token, const QByteArray &stamp);
    void revoke(const QString &token);
    qint64 lifetimeMs() const { return m_lifetimeMs; }

private:
    struct Session
    {
        QString username;
        QByteArray stamp;
        QDeadlineTimer expires;
    };

    void prune();

    QHash<QString, Session> m_sessions;
    qint64 m_lifetimeMs;
};

// Slows down password guessing: after a few failures from one address every
// further failure doubles the wait.
class LoginThrottle
{
public:
    explicit LoginThrottle(int freeAttempts = 5, qint64 firstDelayMs = 30000, qint64 maxDelayMs = 15 * 60 * 1000);

    // False while the address has to wait; retryAfterMs tells how long
    bool allowed(const QHostAddress &address, qint64 *retryAfterMs = nullptr) const;
    void recordFailure(const QHostAddress &address);
    void recordSuccess(const QHostAddress &address);

private:
    struct Entry
    {
        int failures = 0;
        QDeadlineTimer blockedUntil;
    };
    static QString key(const QHostAddress &address);

    QHash<QString, Entry> m_entries;
    int m_freeAttempts;
    qint64 m_firstDelayMs;
    qint64 m_maxDelayMs;
};
