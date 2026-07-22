// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <chrono>
#include <functional>
#include <memory>

namespace ao::rt
{
  class NotificationService;
}

namespace ao::uimodel
{
  using ActivityStatusClock = std::function<std::chrono::steady_clock::time_point()>;

  struct ActivityStatusViewModelOptions final
  {
    rt::LibraryChanges const* libraryChanges = nullptr;
    ActivityStatusClock clock{};
    bool emitInitialState = true;
  };

  class ActivityStatusViewModel final
  {
  public:
    ActivityStatusViewModel(rt::NotificationService& notifications,
                            std::function<void(ActivityStatusViewState const&)> onRender,
                            ActivityStatusViewModelOptions options = {});

    ActivityStatusViewModel(ActivityStatusViewModel const&) = delete;
    ActivityStatusViewModel& operator=(ActivityStatusViewModel const&) = delete;
    ActivityStatusViewModel(ActivityStatusViewModel&&) = delete;
    ActivityStatusViewModel& operator=(ActivityStatusViewModel&&) = delete;

    ~ActivityStatusViewModel();

    ActivityStatusViewState const& viewState() const noexcept;
    bool autoDismissCompactIfDue();
    void autoDismissCompact();
    void dismissCompact();
    void hideDetailNotification(rt::NotificationId id);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
