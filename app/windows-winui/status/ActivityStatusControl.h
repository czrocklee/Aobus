// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/NotificationIds.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::uimodel
{
  class ActivityStatusViewModel;
  struct ActivityDetailItem;
  struct ActivityStatusViewState;
  struct ActivityTaskDetail;
}

namespace ao::winui
{
  struct ActivityStatusControlConfig final
  {
    winrt::Microsoft::UI::Xaml::Controls::Grid root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button detailButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing spinner{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon statusIcon{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressBar progress{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button dismissButton{nullptr};
    bool reserveIdle = false;
  };

  class ActivityStatusControl final
  {
  public:
    explicit ActivityStatusControl(ActivityStatusControlConfig config);
    ~ActivityStatusControl();

    ActivityStatusControl(ActivityStatusControl const&) = delete;
    ActivityStatusControl& operator=(ActivityStatusControl const&) = delete;
    ActivityStatusControl(ActivityStatusControl&&) = delete;
    ActivityStatusControl& operator=(ActivityStatusControl&&) = delete;

    void bind(std::shared_ptr<rt::AppRuntime> runtimePtr);
    void unbind();

  private:
    struct DetailDismissRegistration final
    {
      winrt::Microsoft::UI::Xaml::Controls::Button button{nullptr};
      winrt::event_token token{};
    };

    void render(uimodel::ActivityStatusViewState const& state);
    void renderDetail(uimodel::ActivityStatusViewState const& state);
    void appendTaskDetail(uimodel::ActivityTaskDetail const& task);
    void appendNotificationDetail(uimodel::ActivityDetailItem const& item);
    void clearDetailRows();
    void syncAutoDismissTimer(uimodel::ActivityCompactState const& compact);
    void cancelAutoDismissTimer();

    winrt::Microsoft::UI::Xaml::Controls::Grid _root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _detailButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing _spinner{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon _statusIcon{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock _label{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressBar _progress{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button _dismissButton{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Flyout _detailFlyout{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel _detailRows{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer _autoDismissTimer{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker _dismissClickRevoker{};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer::Tick_revoker _autoDismissTickRevoker{};
    std::vector<DetailDismissRegistration> _detailDismissRegistrations;
    std::shared_ptr<rt::AppRuntime> _runtimePtr;
    std::unique_ptr<uimodel::ActivityStatusViewModel> _viewModelPtr;
    std::optional<uimodel::ActivityCompactState> _optScheduledCompact;
    std::uint64_t _autoDismissGeneration = 0;
    bool _reserveIdle = false;
  };
} // namespace ao::winui
