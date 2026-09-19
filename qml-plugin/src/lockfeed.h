#pragma once

#include <QByteArray>
#include <QImage>
#include <QQuickItem>
#include <QString>
#include <QTimer>
#include <QVector>

class QSocketNotifier;
class QSGTexture;

/* Displays the frame stream that the OWE renderer publishes on its lock feed
 * socket. The renderer owns the buffers and sends them as memfds, so this item
 * only maps them and copies the newest frame into a QImage for upload. */
class LockFeed : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)

public:
    enum FillMode { Stretch, PreserveAspectFit, PreserveAspectCrop };
    Q_ENUM(FillMode)

    explicit LockFeed(QQuickItem *parent = nullptr);
    ~LockFeed() override;

    QString socketPath() const;
    void setSocketPath(const QString &path);

    bool active() const;
    void setActive(bool active);

    FillMode fillMode() const;
    void setFillMode(FillMode mode);

    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;

protected:
    void componentComplete() override;

signals:
    void socketPathChanged();
    void activeChanged();
    void fillModeChanged();

private:
    void connectSocket();
    void disconnectSocket();
    void retrySocket();
    void readSocket();
    bool handleMessage(const QByteArray &message, const QVector<int> &fds);
    void resetMaps();

    QString m_socketPath;
    bool m_active = true;
    FillMode m_fillMode = PreserveAspectCrop;

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_retry;
    QByteArray m_buffer;
    QVector<int> m_pendingFds;

    struct FrameSlot {
        void *map = nullptr;
        size_t size = 0;
    };
    QVector<FrameSlot> m_slots;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;

    QImage m_frame;
};
