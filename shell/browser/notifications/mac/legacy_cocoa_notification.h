// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_LEGACY_COCOA_NOTIFICATION_H_
#define ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_LEGACY_COCOA_NOTIFICATION_H_

#import <Foundation/Foundation.h>

#include <map>
#include <string>

#include "shell/browser/notifications/notification.h"

namespace electron {

// A notification shown with NSUserNotification, for macOS versions before
// 10.14 where the User Notifications framework doesn't exist.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

class LegacyCocoaNotification : public Notification {
 public:
  LegacyCocoaNotification(NotificationDelegate* delegate,
                          NotificationPresenter* presenter);
  ~LegacyCocoaNotification() override;

  // Notification:
  void Show(const NotificationOptions& options) override;
  void Dismiss() override;

  void NotificationDisplayed();
  void NotificationReplied(const std::string& reply);
  void NotificationActivated();
  void NotificationActivated(NSUserNotificationAction* action);
  void NotificationDismissed();

  NSUserNotification* notification() const { return notification_; }

 private:
  void LogAction(const char* action);

  NSUserNotification* __strong notification_;
  std::map<std::string, unsigned> additional_action_indices_;
  unsigned action_index_;
};

// -Wdeprecated-declarations
#pragma clang diagnostic pop

}  // namespace electron

#endif  // ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_LEGACY_COCOA_NOTIFICATION_H_
