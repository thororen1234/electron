// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/browser/notifications/mac/legacy_notification_presenter_mac.h"

#include "base/logging.h"
#include "shell/browser/notifications/mac/legacy_cocoa_notification.h"
#include "shell/browser/notifications/mac/legacy_notification_center_delegate.h"

// NSUserNotification is deprecated, but the User Notifications framework
// doesn't exist before macOS 10.14.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace electron {

LegacyCocoaNotification* LegacyNotificationPresenterMac::GetNotification(
    NSUserNotification* ns_notification) {
  for (Notification* notification : notifications()) {
    auto* native_notification =
        static_cast<LegacyCocoaNotification*>(notification);
    if ([native_notification->notification().identifier
            isEqual:ns_notification.identifier])
      return native_notification;
  }

  if (electron::debug_notifications) {
    LOG(INFO) << "Could not find notification for "
              << [ns_notification.identifier UTF8String];
  }

  return nullptr;
}

LegacyNotificationPresenterMac::LegacyNotificationPresenterMac()
    : notification_center_delegate_(
          [[LegacyNotificationCenterDelegate alloc] initWithPresenter:this]) {
  NSUserNotificationCenter.defaultUserNotificationCenter.delegate =
      notification_center_delegate_;
}

LegacyNotificationPresenterMac::~LegacyNotificationPresenterMac() {
  NSUserNotificationCenter.defaultUserNotificationCenter.delegate = nil;
}

Notification* LegacyNotificationPresenterMac::CreateNotificationObject(
    NotificationDelegate* delegate) {
  return new LegacyCocoaNotification(delegate, this);
}

}  // namespace electron

#pragma clang diagnostic pop
