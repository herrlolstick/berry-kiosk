#include "sdnotify.h"

#include <QString>
#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace berry {

void sdNotify(const QByteArray &state)
{
#ifdef Q_OS_LINUX
    const QByteArray path = qgetenv("NOTIFY_SOCKET");
    if (path.isEmpty() || path.size() >= int(sizeof(sockaddr_un::sun_path)))
        return;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
    if (address.sun_path[0] == '@')
        address.sun_path[0] = '\0'; // abstract socket
    const socklen_t length = socklen_t(offsetof(sockaddr_un, sun_path) + size_t(path.size()));

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    ::sendto(fd, state.constData(), size_t(state.size()), MSG_NOSIGNAL,
             reinterpret_cast<const sockaddr *>(&address), length);
    ::close(fd);
#else
    Q_UNUSED(state);
#endif
}

int watchdogIntervalMs()
{
    bool ok = false;
    const qint64 usec = qEnvironmentVariable("WATCHDOG_USEC").toLongLong(&ok);
    if (!ok || usec <= 0)
        return 0;
    return int(qBound(qint64(1000), usec / 2000, qint64(60 * 60 * 1000)));
}

} // namespace berry
