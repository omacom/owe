#include "lockfeed.h"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSocketNotifier>
#include <QHash>

/* Items on multiple outputs share one CPU copy of each decoded frame. The
 * cache lives only as long as its clients and is keyed by the ring's memfd,
 * so a renderer restart cannot reuse an old frame sequence. GUI thread only. */
struct LockFeedFrameCache {
    quint64 seq = 0;
    QImage image;
};

namespace {

QSharedPointer<LockFeedFrameCache> frameCache(const QString &key) {
    static QHash<QString, QWeakPointer<LockFeedFrameCache>> caches;
    for (auto it = caches.begin(); it != caches.end();) {
        if (it.value().isNull()) it = caches.erase(it);
        else ++it;
    }
    auto cache = caches.value(key).toStrongRef();
    if (!cache) {
        cache = QSharedPointer<LockFeedFrameCache>::create();
        caches.insert(key, cache.toWeakRef());
    }
    return cache;
}

class FrameNode : public QSGSimpleTextureNode {
public:
    qint64 frameKey = 0;
};

constexpr quint32 kMagic = 0x4645574Fu;
constexpr quint32 kVersion = 1;

enum MessageType {
    Hello = 1,
    Frame = 2,
    Ack = 3,
};

struct FeedMessage {
    quint32 magic;
    quint32 version;
    quint32 type;
    quint32 slot;
    quint32 width;
    quint32 height;
    quint32 stride;
    quint32 format;
    quint64 seq;
};

static_assert(sizeof(FeedMessage) == 40, "feed message size");

FeedMessage readMessage(const QByteArray &data) {
    FeedMessage message;
    memcpy(&message, data.constData(), sizeof(message));
    return message;
}

QByteArray writeMessage(quint32 type, quint32 slot, quint64 seq) {
    FeedMessage message;
    memset(&message, 0, sizeof(message));
    message.magic = kMagic;
    message.version = kVersion;
    message.type = type;
    message.slot = slot;
    message.seq = seq;
    return QByteArray(reinterpret_cast<const char *>(&message), sizeof(message));
}

QString defaultSocketPath() {
    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) {
        return QStringLiteral("/run/user/%1/owe/lock-feed.sock").arg(getuid());
    }
    return QString::fromLocal8Bit(runtime) + QStringLiteral("/owe/lock-feed.sock");
}

} // namespace

LockFeed::LockFeed(QQuickItem *parent)
    : QQuickItem(parent), m_socketPath(defaultSocketPath()) {
    setFlag(ItemHasContents, true);
    m_retry.setSingleShot(true);
    m_retry.setInterval(500);
    connect(&m_retry, &QTimer::timeout, this, &LockFeed::connectSocket);
    connect(this, &QQuickItem::windowChanged, this, [this]() {
        if (isComponentComplete() && m_active) {
            connectSocket();
        }
    });
}

LockFeed::~LockFeed() {
    disconnectSocket();
}

void LockFeed::componentComplete() {
    QQuickItem::componentComplete();
    if (m_active) {
        connectSocket();
    }
}

QString LockFeed::socketPath() const {
    return m_socketPath;
}

void LockFeed::setSocketPath(const QString &path) {
    if (m_socketPath == path) {
        return;
    }
    m_socketPath = path;
    if (m_active) {
        disconnectSocket();
        connectSocket();
    }
    emit socketPathChanged();
}

bool LockFeed::active() const {
    return m_active;
}

void LockFeed::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    if (m_active) {
        connectSocket();
    } else {
        disconnectSocket();
    }
    emit activeChanged();
}

LockFeed::FillMode LockFeed::fillMode() const {
    return m_fillMode;
}

void LockFeed::setFillMode(FillMode mode) {
    if (m_fillMode == mode) {
        return;
    }
    m_fillMode = mode;
    update();
    emit fillModeChanged();
}

void LockFeed::connectSocket() {
    struct sockaddr_un addr;
    const QByteArray path = m_socketPath.toLocal8Bit();
    int fd;
    if (!m_active || m_fd >= 0 || path.isEmpty()) {
        return;
    }
    if (path.size() + 1 > (int)sizeof(addr.sun_path)) {
        return;
    }
    fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        m_retry.start();
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.constData(), (size_t)path.size() + 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        if (m_active) {
            m_retry.start();
        }
        return;
    }
    m_retry.stop();
    m_fd = fd;
    m_buffer.clear();
    m_pendingFds.clear();
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &LockFeed::readSocket);
    m_writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, [this]() {
        if (!flushAcks()) retrySocket();
    });
}

void LockFeed::disconnectSocket() {
    m_retry.stop();
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_writeNotifier) {
        m_writeNotifier->setEnabled(false);
        m_writeNotifier->deleteLater();
        m_writeNotifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    for (int fd : m_pendingFds) {
        ::close(fd);
    }
    m_pendingFds.clear();
    m_buffer.clear();
    m_acks.clear();
    resetMaps();
    m_frame = QImage();
    update();
}

void LockFeed::retrySocket() {
    disconnectSocket();
    if (m_active) {
        m_retry.start();
    }
}

void LockFeed::resetMaps() {
    m_frameCache.clear();
    for (const FrameSlot &slot : m_slots) {
        if (slot.map && slot.size) {
            munmap(slot.map, slot.size);
        }
    }
    m_slots.clear();
    m_width = 0;
    m_height = 0;
    m_stride = 0;
}

bool LockFeed::flushAcks() {
    while (!m_acks.isEmpty()) {
        const ssize_t bytes = send(m_fd, m_acks.constData(), (size_t)m_acks.size(), MSG_NOSIGNAL);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            m_writeNotifier->setEnabled(true);
            return true;
        }
        if (bytes <= 0) return false;
        m_acks.remove(0, bytes);
    }
    if (m_writeNotifier) m_writeNotifier->setEnabled(false);
    return true;
}

void LockFeed::readSocket() {
    alignas(struct cmsghdr) char control[CMSG_SPACE(sizeof(int) * 8)];
    char data[sizeof(FeedMessage)];
    for (;;) {
        struct iovec iov;
        struct msghdr header;
        ssize_t n;
        iov.iov_base = data;
        /* Stop at each message boundary so descriptors stay with their HELLO. */
        iov.iov_len = sizeof(FeedMessage) - (size_t)m_buffer.size();
        memset(&header, 0, sizeof(header));
        header.msg_iov = &iov;
        header.msg_iovlen = 1;
        header.msg_control = control;
        header.msg_controllen = sizeof(control);
        n = recvmsg(m_fd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n == 0) {
            retrySocket();
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            retrySocket();
            return;
        }
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header); cmsg; cmsg = CMSG_NXTHDR(&header, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
                const int count = (int)(bytes / sizeof(int));
                const int *fds = reinterpret_cast<const int *>(CMSG_DATA(cmsg));
                for (int i = 0; i < count; i++) {
                    m_pendingFds.append(fds[i]);
                }
            }
        }
        if (header.msg_flags & MSG_CTRUNC) {
            retrySocket();
            return;
        }
        m_buffer.append(data, (int)n);
        if (m_buffer.size() == (int)sizeof(FeedMessage)) {
            const bool ok = handleMessage(m_buffer, m_pendingFds);
            for (int fd : m_pendingFds) {
                ::close(fd);
            }
            m_pendingFds.clear();
            m_buffer.clear();
            if (!ok) {
                retrySocket();
                return;
            }
        }
    }
}

bool LockFeed::handleMessage(const QByteArray &message, const QVector<int> &fds) {
    const FeedMessage msg = readMessage(message);
    if (msg.magic != kMagic || msg.version != kVersion) {
        return false;
    }
    if (msg.type == Hello) {
        if (msg.slot == 0 || msg.slot > 8 || msg.slot != (quint32)fds.size() ||
            msg.width == 0 || msg.width > 3840 || msg.height == 0 || msg.height > 2160 ||
            msg.stride != msg.width * 4 || msg.format != 0) {
            return false;
        }
        resetMaps();
        m_width = (int)msg.width;
        m_height = (int)msg.height;
        m_stride = (int)msg.stride;
        QString cacheKey;
        for (int fd : fds) {
            FrameSlot slot;
            struct stat st;
            slot.size = (size_t)m_stride * (size_t)m_height;
            if (fstat(fd, &st) != 0 || st.st_size < (off_t)slot.size) {
                return false;
            }
            if (cacheKey.isEmpty()) {
                cacheKey = QStringLiteral("%1:%2:%3:%4")
                    .arg(qulonglong(st.st_dev)).arg(qulonglong(st.st_ino)).arg(m_width).arg(m_height);
            }
            slot.map = mmap(nullptr, slot.size, PROT_READ, MAP_SHARED, fd, 0);
            if (slot.map == MAP_FAILED) {
                return false;
            }
            m_slots.append(slot);
        }
        m_frameCache = frameCache(cacheKey);
        m_frame = QImage();
        update();
        return true;
    }
    if (msg.type == Frame && fds.isEmpty()) {
        const int slot = (int)msg.slot;
        if (slot < 0 || slot >= m_slots.size() || !m_slots[slot].map || m_width <= 0 || m_height <= 0) {
            return false;
        }
        if (!m_frameCache || msg.seq == 0) return false;
        if (m_frameCache->seq != msg.seq || m_frameCache->image.isNull()) {
            QImage image(static_cast<const uchar *>(m_slots[slot].map), m_width, m_height,
                         m_stride, QImage::Format_RGBA8888);
            QImage copy = image.copy();
            if (copy.isNull()) return false;
            m_frameCache->image = copy;
            m_frameCache->seq = msg.seq;
        }
        m_frame = m_frameCache->image;
        update();
        const QByteArray ack = writeMessage(Ack, (quint32)slot, msg.seq);
        if (m_acks.size() + ack.size() > 8 * (int)sizeof(FeedMessage)) return false;
        m_acks.append(ack);
        return flushAcks();
    }
    return false;
}

QSGNode *LockFeed::updatePaintNode(QSGNode *node, UpdatePaintNodeData *) {
    if (m_frame.isNull() || !window()) {
        delete node;
        return nullptr;
    }
    FrameNode *textureNode = static_cast<FrameNode *>(node);
    if (!textureNode) {
        textureNode = new FrameNode();
        textureNode->setOwnsTexture(true);
        textureNode->setFiltering(QSGTexture::Linear);
    }
    if (textureNode->frameKey != m_frame.cacheKey()) {
        QSGTexture *texture = window()->createTextureFromImage(m_frame);
        if (!texture) {
            delete textureNode;
            return nullptr;
        }
        texture->setFiltering(QSGTexture::Linear);
        textureNode->setTexture(texture);
        textureNode->frameKey = m_frame.cacheKey();
    }

    const QRectF bounds = boundingRect();
    const qreal imageWidth = m_frame.width();
    const qreal imageHeight = m_frame.height();
    if (imageWidth <= 0 || imageHeight <= 0) {
        delete textureNode;
        return nullptr;
    }
    if (m_fillMode == Stretch) {
        textureNode->setRect(bounds);
        textureNode->setSourceRect(QRectF(0, 0, imageWidth, imageHeight));
    } else if (m_fillMode == PreserveAspectFit) {
        const qreal scale = qMin(bounds.width() / imageWidth, bounds.height() / imageHeight);
        const qreal width = imageWidth * scale;
        const qreal height = imageHeight * scale;
        textureNode->setRect(QRectF((bounds.width() - width) / 2.0, (bounds.height() - height) / 2.0,
                                    width, height));
        textureNode->setSourceRect(QRectF(0, 0, imageWidth, imageHeight));
    } else {
        QRectF source(0, 0, imageWidth, imageHeight);
        const qreal imageAspect = imageWidth / imageHeight;
        const qreal boundsAspect = bounds.width() / bounds.height();
        if (imageAspect > boundsAspect) {
            const qreal width = imageHeight * boundsAspect;
            source.setX((imageWidth - width) / 2.0);
            source.setWidth(width);
        } else {
            const qreal height = imageWidth / boundsAspect;
            source.setY((imageHeight - height) / 2.0);
            source.setHeight(height);
        }
        textureNode->setRect(bounds);
        textureNode->setSourceRect(source);
    }
    return textureNode;
}
