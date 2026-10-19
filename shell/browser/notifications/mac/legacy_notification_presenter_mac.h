// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright (c) 2013 Adam Roben <adam@roben.org>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE-CHROMIUM file.

#ifndef ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_LEGACY_NOTIFICATION_PRESENTER_MAC_H_
#define ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_LEGACY_NOTIFICATION_PRESENTER_MAC_H_

#include "shell/browser/notifications/mac/legacy_notification_center_delegate.h"
#include "shell/browser/notifications/notification_presenter.h"

namespace electron {

class LegacyCocoaNotification;

// Shows notifications with NSUserNotification, for macOS versions before 10.14
// where the User Notifications framework doesn't exist. Delivered
// notifications can't be listed or removed.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

class LegacyNotificationPresenterMac : public NotificationPresenter {
 public:
  LegacyCocoaNotification* GetNotification(
      NSUserNotification* ns_notification);

  LegacyNotificationPresenterMac();
  ~LegacyNotificationPresenterMac() override;

 private:
  // NotificationPresenter
  Notification* CreateNotificationObject(
      NotificationDelegate* delegate) override;

  LegacyNotificationCenterDelegate* __strong notification_center_delegate_;
};

// -Wdeprecated-declarations
#pragma clang diagnostic pop

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_LEGACY_NOTIFICATION_PRESENTER_MAC_H_
