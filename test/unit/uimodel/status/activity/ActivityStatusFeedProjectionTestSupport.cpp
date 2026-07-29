// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/uimodel/status/activity/ActivityStatusFeedProjectionTestSupport.h"

#include <ao/rt/NotificationIds.h>
#include <ao/rt/NotificationState.h>

#include <memory>
#include <utility>

namespace ao::uimodel::test
{
  rt::NotificationFeedUpdate postedUpdate(rt::NotificationFeedState snapshot, rt::NotificationId const id)
  {
    auto feedPtr = std::make_shared<rt::NotificationFeedState const>(std::move(snapshot));
    return rt::NotificationFeedUpdate{
      .mutationKind = rt::NotificationFeedMutationKind::Posted,
      .id = id,
      .feedPtr = std::move(feedPtr),
    };
  }

  rt::NotificationFeedUpdate expiredUpdate(rt::NotificationFeedState snapshot, rt::NotificationId const id)
  {
    auto feedPtr = std::make_shared<rt::NotificationFeedState const>(std::move(snapshot));
    return rt::NotificationFeedUpdate{
      .mutationKind = rt::NotificationFeedMutationKind::Expired,
      .id = id,
      .feedPtr = std::move(feedPtr),
    };
  }
} // namespace ao::uimodel::test
