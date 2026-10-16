// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/keep_awake.hpp"

#include <QCoreApplication>
#include <QThread>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#endif

#include <optional>

namespace trackknife::bench {
namespace {

// The one platform hold, taken with the first job and let go with the last.
// Only the GUI thread touches it.
class Inhibitor final {
  public:
    void take(const QString& reason) {
#ifdef __APPLE__
        const auto name = reason.toCFString();
        IOPMAssertionID id = kIOPMNullAssertionID;
        if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                        kIOPMAssertionLevelOn, name, &id) == kIOReturnSuccess) {
            assertion_ = id;
        }
        CFRelease(name);
#else
        // logind answers with a descriptor; the inhibitor lasts while it is
        // open. Asked without waiting: the system bus may be slow or absent,
        // and a job never waits for it.
        auto call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.login1"), QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"), QStringLiteral("Inhibit"));
        call << QStringLiteral("idle") << QStringLiteral("Melody") << reason
             << QStringLiteral("block");
        auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call),
                                                    QCoreApplication::instance());
        const auto asked = ++generation_;
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                         [this, asked](QDBusPendingCallWatcher* finished) {
                             const QDBusPendingReply<QDBusUnixFileDescriptor> reply{*finished};
                             // Let go before it came: dropped with the reply.
                             if (reply.isValid() && asked == generation_ && held_) {
                                 descriptor_ = reply.value();
                             }
                             finished->deleteLater();
                         });
#endif
        held_ = true;
    }

    void release() {
        held_ = false;
#ifdef __APPLE__
        if (assertion_ != kIOPMNullAssertionID) {
            IOPMAssertionRelease(assertion_);
            assertion_ = kIOPMNullAssertionID;
        }
#else
        ++generation_;
        descriptor_.reset();
#endif
    }

  private:
    bool held_{false};
#ifdef __APPLE__
    IOPMAssertionID assertion_{kIOPMNullAssertionID};
#else
    quint64 generation_{0U};
    std::optional<QDBusUnixFileDescriptor> descriptor_;
#endif
};

Inhibitor& inhibitor() {
    static Inhibitor one;
    return one;
}

int& count() {
    static int held = 0;
    return held;
}

} // namespace

KeepAwake::Hold KeepAwake::hold(const QString& reason) {
    Q_ASSERT(QCoreApplication::instance() == nullptr ||
             QThread::currentThread() == QCoreApplication::instance()->thread());
    if (count()++ == 0) {
        inhibitor().take(reason);
    }
    // The hold's own deleter lets it go; the pointer carries nothing.
    return {nullptr, [](void*) {
                if (--count() == 0) {
                    inhibitor().release();
                }
            }};
}

int KeepAwake::holds() { return count(); }

} // namespace trackknife::bench
