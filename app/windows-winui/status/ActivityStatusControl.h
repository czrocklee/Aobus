// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationIds.h>
#include <ao/uimodel/presentation/PresentationText.h>
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
  class LibraryJobs;
  class NotificationService;
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
    i18n::MessageCatalog textCatalog;
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

    void bind(rt::NotificationService& notifications, rt::LibraryJobs& libraryJobs);
    void unbind() noexcept;

  private:
    /// Blank the widget between bindings. Only a rebind has anything to show.
    void resetPresentation();

    void render(uimodel::ActivityStatusViewState const& state);
    void renderDetail(uimodel::ActivityStatusViewState const& state);
    void appendTaskDetail(uimodel::ActivityTaskDetail const& task);
    void appendNotificationDetail(uimodel::ActivityDetailItem const& item);
    void clearDetailRows() noexcept;
    void syncAutoDismissTimer(uimodel::ActivityCompactState const& compact);
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
    rt::NotificationService* _notifications = nullptr;
    rt::LibraryJobs* _libraryJobs = nullptr;
    std::unique_ptr<uimodel::ActivityStatusViewModel> _viewModelPtr;
    std::optional<uimodel::ActivityCompactState> _optScheduledCompact;
    std::uint64_t _autoDismissGeneration = 0;
    bool _reserveIdle = false;
  };
} // namespace ao::winui
