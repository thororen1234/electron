// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright (c) 2013 Adam Roben <adam@roben.org>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE-CHROMIUM file.

#ifndef ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_NOTIFICATION_PRESENTER_MAC_H_
#define ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_NOTIFICATION_PRESENTER_MAC_H_

#include "chrome/common/notifications/notification_image_retainer.h"
#include "shell/browser/notifications/mac/notification_center_delegate.h"
#include "shell/browser/notifications/notification_presenter.h"

#import <UserNotifications/UserNotifications.h>

// The User Notifications framework needs macOS 10.14. This is only used on
// macOS 10.14 and later (see NotificationPresenter::Create()), so its uses
// are not checked individually.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

namespace electron {

class CocoaNotification;

class NotificationPresenterMac : public NotificationPresenter {
 public:
  CocoaNotification* GetNotification(
      UNNotificationRequest* un_notification_request);

  NotificationPresenterMac();
  ~NotificationPresenterMac() override;

  // NotificationPresenter
  void GetDeliveredNotifications(
      GetDeliveredNotificationsCallback callback) override;
  void RemoveDeliveredNotifications(
      const std::vector<std::string>& identifiers) override;
  void RemoveAllDeliveredNotifications() override;
  void RemoveDeliveredNotificationsByGroupId(
      const std::string& group_id) override;

  NotificationImageRetainer* image_retainer() { return image_retainer_.get(); }
  scoped_refptr<base::SequencedTaskRunner> image_task_runner() {
    return image_task_runner_;
  }

 private:
  // NotificationPresenter
  Notification* CreateNotificationObject(
      NotificationDelegate* delegate) override;

  NotificationCenterDelegate* __strong notification_center_delegate_;
  std::unique_ptr<NotificationImageRetainer> image_retainer_;
  scoped_refptr<base::SequencedTaskRunner> image_task_runner_;
};

}  // namespace electron

#pragma clang diagnostic pop

#endif  // ELECTRON_SHELL_BROWSER_NOTIFICATIONS_MAC_NOTIFICATION_PRESENTER_MAC_H_
