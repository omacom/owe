#include <QQmlExtensionPlugin>
#include <qqml.h>

#include "lockfeed.h"

class OweLockFeedPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override {
        qmlRegisterType<LockFeed>(uri, 1, 0, "LockFeed");
    }
};

#include "plugin.moc"
