// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#import <Foundation/Foundation.h>

#include <chrono>
#include <functional>
#include <optional>

using AobusActivityExpirationRefresh = std::function<std::optional<std::chrono::steady_clock::duration>()>;
using AobusActivityTimerScheduler = NSTimer* (^)(NSTimeInterval, void (^)(NSTimer*));

// Main-thread owner of the compact activity wakeup. Refresh returns the model's
// remaining lifetime after dismissing any due presentation; nullopt cancels it.
@interface AobusActivityExpirationTimer : NSObject
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;
- (instancetype)initWithRefresh:(AobusActivityExpirationRefresh)refresh;
// The scheduler returns a one-shot timer whose callback is delivered later.
- (instancetype)initWithRefresh:(AobusActivityExpirationRefresh)refresh
                      scheduler:(AobusActivityTimerScheduler)scheduler;
- (void)refresh;
- (void)invalidate;
@end
