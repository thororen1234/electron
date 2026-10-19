// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_NOTIFICATION_CENTER_DELEGATE_H_
#define ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_NOTIFICATION_CENTER_DELEGATE_H_

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include "base/memory/raw_ptr.h"

// The User Notifications framework needs macOS 10.14. This is only used on
// macOS 10.14 and later (see NotificationPresenter::Create()), so its uses
// are not checked individually.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

namespace electron {
class NotificationPresenterMac;
}

@interface NotificationCenterDelegate
    : NSObject <UNUserNotificationCenterDelegate> {
 @private
  raw_ptr<electron::NotificationPresenterMac> presenter_;
}
- (instancetype)initWithPresenter:
    (electron::NotificationPresenterMac*)presenter;
@end

#pragma clang diagnostic pop

#endif  // ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_NOTIFICATION_CENTER_DELEGATE_H_
