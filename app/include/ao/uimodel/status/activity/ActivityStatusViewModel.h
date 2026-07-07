// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace ao::rt
{
  class LibraryChanges;
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
    bool hasPendingAutoDismiss() const noexcept;
    bool expireTransientIfDue();
    void expireTransient();
    void dismissCompact();
    void hideDetailNotificationFromActivity(rt::NotificationId id);
    void onLibraryTaskProgress(std::string message, double fraction);
    void onLibraryTaskCompleted(std::size_t count);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel
