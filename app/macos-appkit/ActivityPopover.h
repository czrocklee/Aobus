// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationIds.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#import <AppKit/AppKit.h>

using AobusActivityDismissHandler = void (^)(void);
using AobusActivityHideNotificationHandler = void (^)(ao::rt::NotificationId identifier);

@interface AobusActivityPopover : NSPopover
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;
- (instancetype)initWithCatalog:(ao::i18n::MessageCatalog const&)catalog
                 dismissHandler:(AobusActivityDismissHandler)dismissHandler
        hideNotificationHandler:(AobusActivityHideNotificationHandler)hideNotificationHandler NS_DESIGNATED_INITIALIZER;
- (void)render:(ao::uimodel::ActivityStatusViewState const&)state maximumHeight:(CGFloat)maximumHeight;
@end
