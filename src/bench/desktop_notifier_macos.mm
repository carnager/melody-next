// SPDX-License-Identifier: GPL-3.0-only

#include "bench/desktop_notifier_macos.hpp"

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QUuid>

#include <memory>
#include <utility>

// Shows the notification while Trackknife is in front too; macOS otherwise
// keeps it out of sight. The background-only setting already decides
// whether there is one to show.
@interface TrackknifeNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation TrackknifeNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList);
}
@end

namespace trackknife::bench {

namespace {

// Back on the receiver's thread, unless it has gone.
void report(const QPointer<QObject>& context, std::function<void(const QString&)> done,
            QString error) {
    if (context.isNull()) {
        return;
    }
    QMetaObject::invokeMethod(
        context.data(),
        [done = std::move(done), error = std::move(error)] { done(error); },
        Qt::QueuedConnection);
}

// The notification center moves an attachment into its own store; the cover
// is the one MPRIS and Now Playing show, so it gets a copy.
[[nodiscard]] NSURL* attachable_copy(const QString& image) {
    if (image.isEmpty()) {
        return nil;
    }
    const auto copy = QDir::temp().filePath(
        QStringLiteral("melody-notification-%1.png")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!QFile::copy(image, copy)) {
        return nil;
    }
    return [NSURL fileURLWithPath:copy.toNSString()];
}

} // namespace

void postMacNotification(QObject* context, const QString& summary, const QString& body,
                         const QString& image, std::function<void(const QString&)> done) {
    QPointer<QObject> guard{context};
    // Outside an app bundle the notification center has no app to post for,
    // and asking it for one ends the process.
    if (NSBundle.mainBundle.bundleIdentifier == nil) {
        report(guard, std::move(done),
               QStringLiteral("macOS shows notifications only for Melody.app"));
        return;
    }
    static TrackknifeNotificationDelegate* delegate = [TrackknifeNotificationDelegate new];
    auto* center = [UNUserNotificationCenter currentNotificationCenter];
    center.delegate = delegate;

    UNMutableNotificationContent* content = [UNMutableNotificationContent new];
    content.title = summary.toNSString();
    content.body = body.toNSString();
    if (NSURL* url = attachable_copy(image); url != nil) {
        UNNotificationAttachment* cover = [UNNotificationAttachment attachmentWithIdentifier:@"cover"
                                                                                         URL:url
                                                                                     options:nil
                                                                                       error:nil];
        if (cover != nil) {
            content.attachments = @[ cover ];
        }
    }
    // One identifier: each track's notification replaces the last.
    UNNotificationRequest* request =
        [UNNotificationRequest requestWithIdentifier:@"org.melody.now-playing"
                                             content:content
                                             trigger:nil];
    auto shared = std::make_shared<std::function<void(const QString&)>>(std::move(done));
    [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert
                          completionHandler:^(BOOL granted, NSError* error) {
                            if (!granted) {
                                report(guard, *shared,
                                       error != nil
                                           ? QString::fromNSString(error.localizedDescription)
                                           : QStringLiteral("Notifications are turned off for "
                                                            "Melody in System Settings"));
                                return;
                            }
                            [center addNotificationRequest:request
                                     withCompletionHandler:^(NSError* failed) {
                                       report(guard, *shared,
                                              failed != nil ? QString::fromNSString(
                                                                  failed.localizedDescription)
                                                            : QString{});
                                     }];
                          }];
}

} // namespace trackknife::bench
