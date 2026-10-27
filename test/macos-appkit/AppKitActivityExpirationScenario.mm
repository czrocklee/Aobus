// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AppKitScenarioSupport.h"
#include "app/macos-appkit/ActivityExpirationTimer.h"
#include <ao/Contract.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>

#include <chrono>
#include <cstddef>

namespace ao::appkit::test
{
  void exerciseActivityExpiration()
  {
    auto catalogRes = i18n::MessageCatalog::create("en");
    AO_INVARIANT(catalogRes, "Activity expiration requires the scenario catalog");
    // No playback session or frame timer exists. Both the model clock and
    // delivery of native one-shot timers are controlled without running the loop.
    for (auto const serviceBeforeExpiry : {false, true})
    {
      auto executor = async::LoopExecutor{};
      auto runtime = async::Runtime{executor, 1};
      auto notifications = rt::NotificationService{runtime};
      auto now = std::chrono::steady_clock::time_point{};
      std::size_t renders = 0;
      auto model = uimodel::ActivityStatusViewModel{
        notifications,
        *catalogRes,
        [&](auto const&) { ++renders; },
        {.clock = [&] { return now; }, .emitInitialState = false},
      };
      auto* const timers = [NSMutableArray<NSTimer*> array];
      auto refreshModel = [&]
      {
        model.tryAutoDismissCompactIfDue();
        return model.compactAutoDismissRemaining();
      };
      __block NSTimeInterval scheduledDelay = 0;
      auto* const expiration = [[AobusActivityExpirationTimer alloc]
        initWithRefresh:refreshModel
              scheduler:^NSTimer*(NSTimeInterval seconds, void (^fire)(NSTimer*)) {
                scheduledDelay = seconds;
                auto* const timer = [NSTimer timerWithTimeInterval:seconds repeats:NO block:fire];
                [timers addObject:timer];
                return timer;
              }];
      auto const key = rt::NotificationReportKey{"appkit.activity.expiration"};
      auto request = rt::NotificationRequest{
        .severity = rt::NotificationSeverity::Info,
        .message = "Temporary A",
        .lifetime = rt::NotificationLifetime::history(),
      };
      notifications.createOrUpdate(key, request);
      [expiration refresh];
      auto* const original = timers.lastObject;
      AO_INVARIANT(original.valid != NO && scheduledDelay == 5, "Temporary A must arm its initial five-second wakeup");

      now += std::chrono::seconds{1};
      request.message = "Temporary B";
      notifications.createOrUpdate(key, request);
      AO_INVARIANT(model.viewState().compact.text == "Temporary B", "The model must present the intermediate B");
      request.message = "Temporary A";
      notifications.createOrUpdate(key, request);
      AO_INVARIANT(renders == 3 && timers.count == 1 && model.viewState().compact.text == "Temporary A",
                   "The model must see A-B-A before the native service sees either intermediate update");

      if (serviceBeforeExpiry)
      {
        [expiration refresh];
        AO_INVARIANT(original.valid == NO && timers.lastObject.valid != NO && scheduledDelay == 5,
                     "A coalesced service must replace the old timer using the model's remaining lifetime");
      }

      now += std::chrono::seconds{4};
      auto const scheduledBeforeEarlyFire = timers.count;
      // Without a service pass this is the original timer; with one, force the
      // replacement timer early to prove the callback independently retains a wakeup.
      [timers.lastObject fire];
      AO_INVARIANT(model.viewState().compact.text == "Temporary A" && timers.count == scheduledBeforeEarlyFire + 1 &&
                     timers.lastObject.valid != NO && scheduledDelay == 1,
                   "An early expiration must preserve A and schedule its remaining second");

      now += std::chrono::seconds{1};
      auto const scheduledBeforeDueFire = timers.count;
      [timers.lastObject fire];
      AO_INVARIANT(model.viewState().compact.kind == uimodel::ActivityStatusKind::Idle &&
                     !model.compactAutoDismissRemaining() && timers.lastObject.valid == NO &&
                     timers.count == scheduledBeforeDueFire,
                   "A must expire at its new deadline without another activity update or periodic render");

      notifications.post(rt::NotificationSeverity::Info, "Temporary C", rt::NotificationLifetime::history());
      [expiration refresh];
      AO_INVARIANT(timers.lastObject.valid != NO, "Another temporary presentation must arm a wakeup");
      [expiration invalidate];
      auto const countAtClose = timers.count;
      [timers.lastObject fire];
      [expiration refresh];
      AO_INVARIANT(timers.lastObject.valid == NO && timers.count == countAtClose,
                   "Invalidating the owner must cancel its timer and prevent rearming");
    }
  }
} // namespace ao::appkit::test
