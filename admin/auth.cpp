#include "auth.h"

#include "state.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPasswordDigestor>
#include <QRandomGenerator>

#include <algorithm>

namespace {

constexpr auto kAlgorithm = "pbkdf2-sha256";
constexpr int kKeyLength = 32;
constexpr int kSaltLength = 16;
constexpr int kMaxSessions = 50;

QByteArray randomBytes(int count)
{
    QByteArray bytes(count, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), count / 4);
    return bytes;
}

QByteArray derive(const QString &password, const QByteArray &salt, int iterations)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(), salt, iterations,
                                              kKeyLength);
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    unsigned char difference = a.size() == b.size() ? 0 : 1;
    const qsizetype length = qMin(a.size(), b.size());
    for (qsizetype i = 0; i < length; ++i)
        difference |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return difference == 0;
}

} // namespace

bool Credentials::isValid() const
{
    return !username.isEmpty() && algorithm == QLatin1String(kAlgorithm) && iterations >= 1000
        && salt.size() >= kSaltLength && hash.size() == kKeyLength;
}

Credentials Credentials::create(const QString &username, const QString &password, int iterations)
{
    Credentials credentials;
    credentials.username = username;
    credentials.algorithm = QLatin1String(kAlgorithm);
    credentials.iterations = iterations;
    credentials.salt = randomBytes(kSaltLength);
    credentials.hash = derive(password, credentials.salt, iterations);
    return credentials;
}

Credentials Credentials::fromJson(const QJsonObject &json)
{
    Credentials credentials;
    credentials.username = json.value(u"username").toString();
    credentials.algorithm = json.value(u"algorithm").toString();
    credentials.iterations = json.value(u"iterations").toInt();
    credentials.salt = QByteArray::fromBase64(json.value(u"salt").toString().toLatin1());
    credentials.hash = QByteArray::fromBase64(json.value(u"hash").toString().toLatin1());
    return credentials;
}

QJsonObject Credentials::toJson() const
{
    return {
        {QStringLiteral("username"), username},
        {QStringLiteral("algorithm"), algorithm},
        {QStringLiteral("iterations"), iterations},
        {QStringLiteral("salt"), QString::fromLatin1(salt.toBase64())},
        {QStringLiteral("hash"), QString::fromLatin1(hash.toBase64())},
    };
}

bool Credentials::verify(const QString &name, const QString &password) const
{
    if (!isValid())
        return false;
    const bool nameMatches = constantTimeEquals(name.toUtf8(), username.toUtf8());
    const bool passwordMatches = constantTimeEquals(derive(password, salt, iterations), hash);
    return nameMatches && passwordMatches;
}

QString credentialsPath(const QString &stateDir)
{
    return QDir(stateDir).filePath(QStringLiteral("auth.json"));
}

Credentials loadCredentials(const QString &path)
{
    return Credentials::fromJson(berry::readJsonFile(path));
}

bool saveCredentials(const QString &path, const Credentials &credentials, QString *error)
{
    if (!berry::writeJsonFile(path, credentials.toJson(), error))
        return false;
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error)
            *error = QStringLiteral("%1: Rechte nicht setzbar").arg(path);
        return false;
    }
    return true;
}

SessionStore::SessionStore(qint64 lifetimeMs)
    : m_lifetimeMs(lifetimeMs)
{
}

QString SessionStore::create(const QString &username, const QByteArray &stamp)
{
    prune();
    while (m_sessions.size() >= kMaxSessions) {
        const auto oldest = std::min_element(m_sessions.begin(), m_sessions.end(), [](const Session &a, const Session &b) {
            return a.expires < b.expires;
        });
        m_sessions.erase(oldest);
    }

    const QString token = QString::fromLatin1(
        randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    m_sessions.insert(token, {username, stamp, QDeadlineTimer(m_lifetimeMs)});
    return token;
}

std::optional<QString> SessionStore::validate(const QString &token, const QByteArray &stamp)
{
    const auto it = m_sessions.find(token);
    if (it == m_sessions.end())
        return std::nullopt;
    if (it->expires.hasExpired() || it->stamp != stamp) {
        m_sessions.erase(it);
        return std::nullopt;
    }
    it->expires = QDeadlineTimer(m_lifetimeMs);
    return it->username;
}

void SessionStore::revoke(const QString &token)
{
    m_sessions.remove(token);
}

void SessionStore::prune()
{
    m_sessions.removeIf([](QHash<QString, Session>::iterator it) { return it.value().expires.hasExpired(); });
}

LoginThrottle::LoginThrottle(int freeAttempts, qint64 firstDelayMs, qint64 maxDelayMs)
    : m_freeAttempts(freeAttempts)
    , m_firstDelayMs(firstDelayMs)
    , m_maxDelayMs(maxDelayMs)
{
}

QString LoginThrottle::key(const QHostAddress &address)
{
    // "::ffff:192.168.1.5" and "192.168.1.5" are the same client
    bool isV4 = false;
    const quint32 v4 = address.toIPv4Address(&isV4);
    return isV4 ? QHostAddress(v4).toString() : address.toString();
}

bool LoginThrottle::allowed(const QHostAddress &address, qint64 *retryAfterMs) const
{
    const auto it = m_entries.constFind(key(address));
    if (it == m_entries.cend() || it->blockedUntil.hasExpired())
        return true;
    if (retryAfterMs)
        *retryAfterMs = it->blockedUntil.remainingTime();
    return false;
}

void LoginThrottle::recordFailure(const QHostAddress &address)
{
    Entry &entry = m_entries[key(address)];
    ++entry.failures;
    if (entry.failures >= m_freeAttempts) {
        const int doublings = qMin(entry.failures - m_freeAttempts, 20);
        const qint64 delay = qMin(m_maxDelayMs, m_firstDelayMs << doublings);
        entry.blockedUntil = QDeadlineTimer(delay);
    }
}

void LoginThrottle::recordSuccess(const QHostAddress &address)
{
    m_entries.remove(key(address));
}
