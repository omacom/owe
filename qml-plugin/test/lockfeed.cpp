#include "lockfeed.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

struct Message {
    quint32 magic = 0x4645574f;
    quint32 version = 1;
    quint32 type = 1;
    quint32 slot = 1;
    quint32 width = 1;
    quint32 height = 1;
    quint32 stride = 4;
    quint32 format = 0;
    quint64 seq = 0;
};

static_assert(sizeof(Message) == 40);

bool sendHello(int peer, int fd, const Message &message, size_t bytes = sizeof(Message)) {
    iovec iov{const_cast<Message *>(&message), bytes};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr header{};
    header.msg_iov = &iov;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    cmsghdr *cmsg = CMSG_FIRSTHDR(&header);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    return sendmsg(peer, &header, MSG_NOSIGNAL) == (ssize_t)bytes;
}

bool sendFrame(int peer, quint64 seq) {
    Message frame;
    frame.type = 2;
    frame.slot = 0;
    frame.seq = seq;
    return send(peer, &frame, sizeof(frame), MSG_NOSIGNAL) == sizeof(frame);
}

int descriptorCount(int fd) {
    int count = 0;
    struct stat target;
    if (fstat(fd, &target) != 0) return -1;
    const QDir directory(QStringLiteral("/proc/self/fd"));
    for (const QString &entry : directory.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const int candidate = entry.toInt(&numeric);
        struct stat st;
        if (numeric && fstat(candidate, &st) == 0 && st.st_dev == target.st_dev && st.st_ino == target.st_ino) {
            count++;
        }
    }
    return count;
}

} // namespace

class LockFeedTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_directory;
    int m_server = -1;
    int m_peer = -1;

    QString socketPath() const { return m_directory.filePath(QStringLiteral("feed.sock")); }

    void startServer() {
        const QByteArray path = socketPath().toLocal8Bit();
        sockaddr_un addr{};
        QVERIFY(path.size() + 1 <= (int)sizeof(addr.sun_path));
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path.constData(), path.size() + 1);
        m_server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        QVERIFY(m_server >= 0);
        QVERIFY(bind(m_server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
        QVERIFY(listen(m_server, 8) == 0);
    }

    void acceptClient() {
        const auto ready = [this]() {
            if (m_peer < 0) {
                m_peer = accept4(m_server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            }
            return m_peer >= 0;
        };
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 3000);
    }

    void verifyAck(quint64 seq) {
        Message ack;
        ssize_t bytes = -1;
        const auto ready = [&]() {
            if (bytes < 0) {
                bytes = recv(m_peer, &ack, sizeof(ack), 0);
            }
            return bytes >= 0;
        };
        QTRY_VERIFY_WITH_TIMEOUT(ready(), 2000);
        QCOMPARE(bytes, (ssize_t)sizeof(ack));
        QCOMPARE(ack.magic, quint32(0x4645574f));
        QCOMPARE(ack.type, quint32(3));
        QCOMPARE(ack.slot, quint32(0));
        QCOMPARE(ack.seq, seq);
    }

private slots:
    void init() {
        QVERIFY(m_directory.isValid());
        startServer();
    }

    void cleanup() {
        if (m_peer >= 0) close(m_peer);
        if (m_server >= 0) close(m_server);
        m_peer = m_server = -1;
        QFile::remove(socketPath());
    }

    void descriptorsStayWithHello() {
        LockFeed item;
        item.setSocketPath(socketPath());
        acceptClient();
        QTemporaryFile first, replacement;
        QVERIFY(first.open() && first.resize(4));
        QVERIFY(replacement.open() && replacement.resize(8));
        QVERIFY(sendHello(m_peer, first.handle(), Message{}));
        QVERIFY(sendFrame(m_peer, 1));
        verifyAck(1);

        Message hello;
        hello.width = 2;
        hello.stride = 8;
        QVERIFY(sendFrame(m_peer, 2));
        QVERIFY(sendHello(m_peer, replacement.handle(), hello));
        QVERIFY(sendFrame(m_peer, 3));
        verifyAck(2);
        verifyAck(3);
        QCOMPARE(descriptorCount(first.handle()), 1);
        QCOMPARE(descriptorCount(replacement.handle()), 1);
    }

    void fragmentedHello() {
        LockFeed item;
        item.setSocketPath(socketPath());
        acceptClient();
        QTemporaryFile backing;
        QVERIFY(backing.open() && backing.resize(4));
        Message hello;
        QVERIFY(sendHello(m_peer, backing.handle(), hello, 7));
        QTest::qWait(25);
        const char *remaining = reinterpret_cast<const char *>(&hello) + 7;
        QVERIFY(send(m_peer, remaining, sizeof(hello) - 7, MSG_NOSIGNAL) == sizeof(hello) - 7);
        QVERIFY(sendFrame(m_peer, 1));
        verifyAck(1);
    }

    void reconnectAfterServerRestart() {
        LockFeed item;
        item.setSocketPath(socketPath());
        acceptClient();
        QTemporaryFile backing;
        QVERIFY(backing.open() && backing.resize(4));
        QVERIFY(sendHello(m_peer, backing.handle(), Message{}));
        QVERIFY(sendFrame(m_peer, 1));
        verifyAck(1);
        close(m_peer);
        close(m_server);
        m_peer = m_server = -1;
        QVERIFY(QFile::remove(socketPath()));
        QTest::qWait(650);
        startServer();
        acceptClient();
        QVERIFY(sendHello(m_peer, backing.handle(), Message{}));
        QVERIFY(sendFrame(m_peer, 2));
        verifyAck(2);
    }

    void inactiveCancelsRetry() {
        LockFeed item;
        item.setSocketPath(socketPath());
        acceptClient();
        close(m_peer);
        m_peer = -1;
        QTest::qWait(100);
        item.setActive(false);
        QTest::qWait(650);
        m_peer = accept4(m_server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        QVERIFY(m_peer < 0 && errno == EAGAIN);
        item.setActive(true);
        acceptClient();
        QTemporaryFile backing;
        QVERIFY(backing.open() && backing.resize(4));
        QVERIFY(sendHello(m_peer, backing.handle(), Message{}));
        QVERIFY(sendFrame(m_peer, 1));
        verifyAck(1);
    }

    void disconnectClosesPartialDescriptors() {
        LockFeed item;
        item.setSocketPath(socketPath());
        acceptClient();
        QTemporaryFile backing;
        QVERIFY(backing.open() && backing.resize(4));
        QVERIFY(sendHello(m_peer, backing.handle(), Message{}, 7));
        QTRY_COMPARE(descriptorCount(backing.handle()), 2);
        close(m_peer);
        m_peer = -1;
        QTRY_COMPARE(descriptorCount(backing.handle()), 1);
        item.setActive(false);
    }

    void invalidHelloClosesDescriptors() {
        LockFeed item;
        item.setSocketPath(socketPath());
        acceptClient();
        QTemporaryFile backing;
        QVERIFY(backing.open() && backing.resize(4));
        Message hello;
        hello.magic = 0;
        QVERIFY(sendHello(m_peer, backing.handle(), hello));
        char byte;
        QTRY_VERIFY(recv(m_peer, &byte, 1, 0) == 0);
        QCOMPARE(descriptorCount(backing.handle()), 1);
        item.setActive(false);
    }
};

QTEST_MAIN(LockFeedTest)
#include "lockfeed.moc"
