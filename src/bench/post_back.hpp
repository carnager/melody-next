// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <utility>

namespace trackknife::bench {

// Hands `work` from a worker thread to the UI thread, to run only while
// `target` still exists there.
//
// Not posted to `target` itself: a worker that checks a QPointer and then
// posts races the UI thread destroying the object in between, and a post to
// an object being destroyed corrupts the event queue -- the crash then comes
// later, in whatever event loop runs next. The application object outlives
// every window, and the check runs on the thread that destroys them.
template <typename Target, typename Work>
void postBack(const QPointer<Target>& target, Work&& work) {
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [target, work = std::forward<Work>(work)]() mutable {
            if (target) {
                work();
            }
        },
        Qt::QueuedConnection);
}

} // namespace trackknife::bench
