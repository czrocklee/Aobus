// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "ActivityExpirationTimer.h"

#include <ao/Contract.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace
{
  constexpr auto kMinimumDelaySeconds = 0.001;
  constexpr auto kMaximumToleranceSeconds = 0.05;
  constexpr auto kToleranceFraction = 0.1;
}

@implementation AobusActivityExpirationTimer {
  AobusActivityExpirationRefresh _refresh;
  AobusActivityTimerScheduler _scheduler;
  NSTimer* _timer;
}
- (instancetype)initWithRefresh:(AobusActivityExpirationRefresh)refresh
{
  return [self initWithRefresh:std::move(refresh)
                     scheduler:^NSTimer*(NSTimeInterval seconds, void (^fire)(NSTimer*)) {
                       auto* const timer = [NSTimer timerWithTimeInterval:seconds repeats:NO block:fire];
                       [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
                       return timer;
                     }];
}
- (instancetype)initWithRefresh:(AobusActivityExpirationRefresh)refresh scheduler:(AobusActivityTimerScheduler)scheduler
{
  self = [super init];

  if (self != nil)
  {
    _refresh = std::move(refresh);
    _scheduler = [scheduler copy];
  }

  return self;
}
- (void)refresh
{
  try
  {
    [_timer invalidate];
    _timer = nil;

    if (auto const optRemaining = _refresh ? _refresh() : std::nullopt; optRemaining)
    {
      auto const seconds = std::max(kMinimumDelaySeconds, std::chrono::duration<double>{*optRemaining}.count());
      __weak AobusActivityExpirationTimer* weakSelf = self;
      _timer = _scheduler(seconds, ^(NSTimer*) { [weakSelf refresh]; });
      _timer.tolerance = std::min(kMaximumToleranceSeconds, seconds * kToleranceFraction);
    }
  }
  catch (...)
  {
    AO_FATAL_EXCEPTION(std::current_exception(), "AppKit activity expiration");
  }
}
- (void)invalidate
{
  [_timer invalidate];
  _timer = nil;
  _refresh = {};
  _scheduler = nil;
}
- (void)dealloc
{
  [_timer invalidate];
}
@end
