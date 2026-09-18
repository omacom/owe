#include "lockfeed.h"

#include <cstring>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSocketNotifier>

namespace {

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
        return QStringLiteral("/run/user/owe/lock-feed.sock");
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
    int fd;
    if (m_fd >= 0 || m_socketPath.isEmpty()) {
        return;
    }
    if (m_socketPath.size() + 1 > (int)sizeof(addr.sun_path)) {
        return;
    }
    fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, m_socketPath.toLocal8Bit().constData(), (size_t)m_socketPath.size() + 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        if (m_active) {
            m_retry.start();
        }
        return;
    }
    m_fd = fd;
    m_buffer.clear();
    m_pendingFds.clear();
    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &LockFeed::readSocket);
}

void LockFeed::disconnectSocket() {
    m_retry.stop();
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    resetMaps();
    m_frame = QImage();
    update();
}

void LockFeed::resetMaps() {
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

void LockFeed::readSocket() {
    char control[CMSG_SPACE(sizeof(int) * 8)];
    struct sockaddr_un addr;
    char data[256];
    for (;;) {
        struct iovec iov;
        struct msghdr header;
        ssize_t n;
        iov.iov_base = data;
        iov.iov_len = sizeof(data);
        memset(&header, 0, sizeof(header));
        header.msg_name = &addr;
        header.msg_namelen = sizeof(addr);
        header.msg_iov = &iov;
        header.msg_iovlen = 1;
        header.msg_control = control;
        header.msg_controllen = sizeof(control);
        n = recvmsg(m_fd, &header, MSG_DONTWAIT);
        if (n == 0) {
            disconnectSocket();
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            disconnectSocket();
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
        m_buffer.append(data, (int)n);
        while (m_buffer.size() >= (int)sizeof(FeedMessage)) {
            const QByteArray chunk = m_buffer.left(sizeof(FeedMessage));
            m_buffer.remove(0, sizeof(FeedMessage));
            handleMessage(chunk, m_pendingFds);
            m_pendingFds.clear();
        }
    }
}

void LockFeed::handleMessage(const QByteArray &message, const QVector<int> &fds) {
    const FeedMessage msg = readMessage(message);
    if (msg.magic != kMagic || msg.version != kVersion) {
        disconnectSocket();
        return;
    }
    if (msg.type == Hello) {
        resetMaps();
        m_width = (int)msg.width;
        m_height = (int)msg.height;
        m_stride = (int)msg.stride;
        for (int fd : fds) {
            FrameSlot slot;
            slot.size = (size_t)m_stride * (size_t)m_height;
            slot.map = mmap(nullptr, slot.size, PROT_READ, MAP_SHARED, fd, 0);
            if (slot.map == MAP_FAILED) {
                slot.map = nullptr;
                slot.size = 0;
            }
            ::close(fd);
            m_slots.append(slot);
        }
        m_frame = QImage();
        update();
        return;
    }
    if (msg.type == Frame) {
        const int slot = (int)msg.slot;
        if (slot < 0 || slot >= m_slots.size() || !m_slots[slot].map || m_width <= 0 || m_height <= 0) {
            return;
        }
        if (m_slots[slot].map && m_width > 0 && m_height > 0) {
            QImage image(m_width, m_height, QImage::Format_RGBA8888);
            for (int row = 0; row < m_height; row++) {
                memcpy(image.scanLine(row),
                       static_cast<const char *>(m_slots[slot].map) + (size_t)row * (size_t)m_stride,
                       (size_t)m_width * 4);
            }
            m_frame = image;
            update();
        }
        const QByteArray ack = writeMessage(Ack, (quint32)slot, msg.seq);
        send(m_fd, ack.constData(), (size_t)ack.size(), MSG_NOSIGNAL);
        return;
    }
}

QSGNode *LockFeed::updatePaintNode(QSGNode *node, UpdatePaintNodeData *) {
    QSGSimpleTextureNode *textureNode = static_cast<QSGSimpleTextureNode *>(node);
    if (!textureNode) {
        textureNode = new QSGSimpleTextureNode();
        textureNode->setOwnsTexture(true);
        textureNode->setFiltering(QSGTexture::Linear);
    }
    if (m_frame.isNull() || !window()) {
        return nullptr;
    }
    QSGTexture *texture = window()->createTextureFromImage(m_frame);
    if (!texture) {
        return nullptr;
    }
    texture->setFiltering(QSGTexture::Linear);
    textureNode->setTexture(texture);

    const QRectF bounds = boundingRect();
    const qreal imageWidth = m_frame.width();
    const qreal imageHeight = m_frame.height();
    if (imageWidth <= 0 || imageHeight <= 0) {
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
