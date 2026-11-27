// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace ao::rt
{
  class LibraryJobs;
  class NotificationService;
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
    i18n::MessageCatalog textCatalog;
    uimodel::ActivityStatusClock clock{};
    // An optional idle timer dedicated to this control on the root's dispatcher.
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer autoDismissTimer{nullptr};
  };

  class ActivityStatusControl final
  {
  public:
    ActivityStatusControl(ActivityStatusControlConfig config,
                          rt::NotificationService& notifications,
                          rt::LibraryJobs const& libraryJobs);
    ~ActivityStatusControl();

    ActivityStatusControl(ActivityStatusControl const&) = delete;
    ActivityStatusControl& operator=(ActivityStatusControl const&) = delete;
    ActivityStatusControl(ActivityStatusControl&&) = delete;
    ActivityStatusControl& operator=(ActivityStatusControl&&) = delete;

  private:
    /// Establish a blank state before the first model snapshot.
    void resetPresentation();
    void stop() noexcept;

    void render(uimodel::ActivityStatusViewState const& state);
    void renderDetail(uimodel::ActivityStatusViewState const& state);
    void appendTaskDetail(uimodel::ActivityTaskDetail const& task);
    void appendNotificationDetail(uimodel::ActivityDetailItem const& item);
    void clearDetailRows() noexcept;
    void syncAutoDismissTimer();
    void cancelAutoDismissTimer() noexcept;

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
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Button::Click_revoker> _detailDismissRevokers;
    i18n::MessageCatalog _textCatalog;
    std::unique_ptr<uimodel::ActivityStatusViewModel> _viewModelPtr;
    std::uint64_t _autoDismissGeneration = 0;
    bool _autoDismissTimerArmed = false;
    bool _reserveIdle = false;
  };
} // namespace ao::winui
