// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "status/ActivityStatusControl.h"

#include "platform/NamedControl.h"
#include "platform/StringResources.h"
#include "platform/WinRtStringResources.h"
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::TextWrapping;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::Flyout;
    using winrt::Microsoft::UI::Xaml::Controls::FontIcon;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::ProgressBar;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollViewer;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode;

    constexpr std::size_t kMaxNotificationDetailRows = 4;
    constexpr double kDetailWidth = 320.0;
    constexpr double kDetailMaxHeight = 360.0;
    constexpr double kDetailDismissButtonSide = 28.0;
    constexpr double kDetailDismissHorizontalPadding = 6.0;
    constexpr double kDetailDismissVerticalPadding = 4.0;
    constexpr double kDetailDismissIconSize = 10.0;
    constexpr double kDetailRowSpacing = 10.0;
    constexpr double kTaskRowSpacing = 6.0;
    constexpr double kNotificationColumnSpacing = 8.0;
    constexpr double kNotificationIconSize = 12.0;
    constexpr double kNotificationIconTopMargin = 4.0;
    constexpr std::wstring_view kInfoGlyph = L"\uE946";
    constexpr std::wstring_view kWarningGlyph = L"\uE7BA";
    constexpr std::wstring_view kErrorGlyph = L"\uEA39";
    constexpr std::wstring_view kDismissGlyph = L"\uE711";
    constexpr std::wstring_view kNotificationGlyph = L"\uEA8F";

    GridLength stars(double const value = 1.0) noexcept
    {
      return {.Value = value, .GridUnitType = GridUnitType::Star};
    }

    GridLength automatic() noexcept
    {
      return {.Value = 1.0, .GridUnitType = GridUnitType::Auto};
    }

    std::wstring_view statusGlyph(uimodel::ActivityStatusKind const kind) noexcept
    {
      switch (kind)
      {
        case uimodel::ActivityStatusKind::Info: return kInfoGlyph;
        case uimodel::ActivityStatusKind::Warning: return kWarningGlyph;
        case uimodel::ActivityStatusKind::Error: return kErrorGlyph;
        case uimodel::ActivityStatusKind::Idle:
        case uimodel::ActivityStatusKind::Processing: return {};
      }

      return {};
    }

    std::wstring_view severityGlyph(rt::NotificationSeverity const severity) noexcept
    {
      switch (severity)
      {
        case rt::NotificationSeverity::Info: return kInfoGlyph;
        case rt::NotificationSeverity::Warning: return kWarningGlyph;
        case rt::NotificationSeverity::Error: return kErrorGlyph;
      }

      return kInfoGlyph;
    }

    TextBlock detailText(std::string_view const text, bool const title = false)
    {
      auto block = TextBlock{};
      block.Text(winrt::to_hstring(text));
      block.TextWrapping(TextWrapping::Wrap);

      if (title)
      {
        block.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
      }

      return block;
    }

    Button detailDismissButton()
    {
      auto const hideLabel = resourceHstring(L"activity_hide_notification");
      auto button = makeNamed<Button>(hideLabel);
      button.MinWidth(kDetailDismissButtonSide);
      button.MinHeight(kDetailDismissButtonSide);
      button.Padding({
        .Left = kDetailDismissHorizontalPadding,
        .Top = kDetailDismissVerticalPadding,
        .Right = kDetailDismissHorizontalPadding,
        .Bottom = kDetailDismissVerticalPadding,
      });
      button.HorizontalAlignment(HorizontalAlignment::Right);
      auto icon = FontIcon{};
      icon.Glyph(winrt::hstring{kDismissGlyph});
      icon.FontSize(kDetailDismissIconSize);
      button.Content(icon);
      ToolTipService::SetToolTip(button, winrt::box_value(hideLabel));
      return button;
    }
  } // namespace

  ActivityStatusControl::ActivityStatusControl(ActivityStatusControlConfig config,
                                               rt::NotificationService& notifications,
                                               rt::LibraryJobs& libraryJobs)
    : _root{std::move(config.root)}
    , _detailButton{std::move(config.detailButton)}
    , _spinner{std::move(config.spinner)}
    , _statusIcon{std::move(config.statusIcon)}
    , _label{std::move(config.label)}
    , _progress{std::move(config.progress)}
    , _dismissButton{std::move(config.dismissButton)}
    , _detailFlyout{Flyout{}}
    , _detailRows{StackPanel{}}
    , _autoDismissTimer{config.autoDismissTimer ? std::move(config.autoDismissTimer)
                                                : _root.DispatcherQueue().CreateTimer()}
    , _textCatalog{std::move(config.textCatalog)}
    , _reserveIdle{config.reserveIdle}
  {
    _detailRows.Spacing(kDetailRowSpacing);

    auto detailScroll = ScrollViewer{};
    detailScroll.Width(kDetailWidth);
    detailScroll.MaxHeight(kDetailMaxHeight);
    detailScroll.Content(_detailRows);
    _detailFlyout.Placement(FlyoutPlacementMode::Top);
    _detailFlyout.Content(detailScroll);

    _dismissClickRevoker = _dismissButton.Click(
      winrt::auto_revoke,
      [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
      {
        if (_viewModelPtr)
        {
          _viewModelPtr->dismissCompact();
        }
      });

    _autoDismissTimer.IsRepeating(false);

    try
    {
      resetPresentation();
      _viewModelPtr = std::make_unique<uimodel::ActivityStatusViewModel>(
        notifications,
        _textCatalog,
        [this](uimodel::ActivityStatusViewState const& state) { render(state); },
        uimodel::ActivityStatusViewModelOptions{
          .libraryJobs = &libraryJobs,
          .clock = std::move(config.clock),
          .emitInitialState = false,
        });
      render(_viewModelPtr->viewState());
    }
    catch (...)
    {
      stop();
      throw;
    }
  }

  ActivityStatusControl::~ActivityStatusControl()
  {
    stop();
  }

  void ActivityStatusControl::stop() noexcept
  {
    _viewModelPtr.reset();
    cancelAutoDismissTimer();
    _detailFlyout.Hide();
    _detailButton.Flyout(nullptr);
    clearDetailRows();
  }

  void ActivityStatusControl::resetPresentation()
  {
    if (_detailFlyout)
    {
      _detailFlyout.Hide();
    }

    render(uimodel::ActivityStatusViewState{});
  }

  void ActivityStatusControl::render(uimodel::ActivityStatusViewState const& state)
  {
    auto const& compact = state.compact;
    bool const idle = compact.kind == uimodel::ActivityStatusKind::Idle;
    bool const reserveIdle = idle && _reserveIdle;
    bool const processing = compact.kind == uimodel::ActivityStatusKind::Processing;
    auto const glyph = reserveIdle ? kNotificationGlyph : statusGlyph(compact.kind);
    auto const idleLabel = resourceString("winui_activity_notifications");

    _root.Visibility(idle && !reserveIdle ? Visibility::Collapsed : Visibility::Visible);
    _spinner.Visibility(processing ? Visibility::Visible : Visibility::Collapsed);
    _spinner.IsActive(processing);
    _statusIcon.Glyph(winrt::hstring{glyph});
    _statusIcon.Visibility(glyph.empty() ? Visibility::Collapsed : Visibility::Visible);
    _label.Text(idle ? winrt::hstring{} : winrt::to_hstring(compact.text));
    // The detail button's content is a panel, from which UI Automation derives
    // no name; it takes the words the strip shows, as the strip itself does.
    auto const accessibleName = idle ? winrt::to_hstring(idleLabel) : _label.Text();
    AutomationProperties::SetName(_root, accessibleName);
    AutomationProperties::SetName(_detailButton, accessibleName);

    if (compact.optProgressFraction)
    {
      _progress.Value(std::clamp(*compact.optProgressFraction, 0.0, 1.0));
      _progress.Visibility(Visibility::Visible);
    }
    else
    {
      _progress.Visibility(Visibility::Collapsed);
    }

    _dismissButton.Visibility(compact.dismissible && !idle ? Visibility::Visible : Visibility::Collapsed);
    bool const openable = uimodel::hasDetailContent(state.detail) && (!idle || reserveIdle);
    // Pointer, keyboard, and automation must agree on whether there is an
    // action. A named but non-hit-testable Button is still UIA-invokable.
    _detailButton.IsEnabled(openable);
    _detailButton.IsHitTestVisible(openable);
    _detailButton.IsTabStop(openable);

    if (!openable)
    {
      _detailFlyout.Hide();
    }

    _detailButton.Flyout(openable ? _detailFlyout : nullptr);
    auto detailToolTip = winrt::Windows::Foundation::IInspectable{nullptr};

    if (openable)
    {
      detailToolTip = winrt::box_value(resourceHstring(L"winui_activity_details"));
    }

    ToolTipService::SetToolTip(_detailButton, detailToolTip);

    renderDetail(state);
    syncAutoDismissTimer();
  }

  void ActivityStatusControl::renderDetail(uimodel::ActivityStatusViewState const& state)
  {
    clearDetailRows();
    auto const& detail = state.detail;

    if (!uimodel::hasDetailContent(detail))
    {
      return;
    }

    if (detail.optLibraryTask)
    {
      appendTaskDetail(*detail.optLibraryTask);
    }

    std::size_t appendedRows = 0;

    for (auto const& item : detail.items)
    {
      if (appendedRows >= kMaxNotificationDetailRows)
      {
        break;
      }

      appendNotificationDetail(item);
      ++appendedRows;
    }
  }

  void ActivityStatusControl::appendTaskDetail(uimodel::ActivityTaskDetail const& task)
  {
    auto row = StackPanel{};
    row.Spacing(kTaskRowSpacing);
    row.Children().Append(detailText(resourceString("library_task_label"), true));
    row.Children().Append(detailText(task.message));

    auto progress = ProgressBar{};
    progress.Minimum(0.0);
    progress.Maximum(1.0);
    progress.Value(std::clamp(task.progressFraction, 0.0, 1.0));
    row.Children().Append(progress);
    _detailRows.Children().Append(row);
  }

  void ActivityStatusControl::appendNotificationDetail(uimodel::ActivityDetailItem const& item)
  {
    auto row = Grid{};
    row.ColumnSpacing(kNotificationColumnSpacing);

    auto iconColumn = ColumnDefinition{};
    iconColumn.Width(automatic());
    row.ColumnDefinitions().Append(iconColumn);
    auto messageColumn = ColumnDefinition{};
    messageColumn.Width(stars());
    row.ColumnDefinitions().Append(messageColumn);
    auto dismissColumn = ColumnDefinition{};
    dismissColumn.Width(automatic());
    row.ColumnDefinitions().Append(dismissColumn);

    auto icon = FontIcon{};
    icon.Glyph(winrt::hstring{severityGlyph(item.severity)});
    icon.FontSize(kNotificationIconSize);
    icon.VerticalAlignment(VerticalAlignment::Top);
    icon.Margin({
      .Left = 0.0,
      .Top = kNotificationIconTopMargin,
      .Right = 0.0,
      .Bottom = 0.0,
    });
    row.Children().Append(icon);

    auto message = detailText(item.message, true);
    Grid::SetColumn(message, 1);
    row.Children().Append(message);

    if (item.dismissible)
    {
      auto button = detailDismissButton();
      Grid::SetColumn(button, 2);
      _detailDismissRevokers.push_back(
        button.Click(winrt::auto_revoke,
                     [this, id = item.id](winrt::Windows::Foundation::IInspectable const&,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                     {
                       if (_viewModelPtr)
                       {
                         _viewModelPtr->hideDetailNotification(id);
                       }
                     }));
      row.Children().Append(button);
    }

    _detailRows.Children().Append(row);
  }

  void ActivityStatusControl::clearDetailRows() noexcept
  {
    _detailDismissRevokers.clear();

    if (_detailRows)
    {
      _detailRows.Children().Clear();
    }
  }

  void ActivityStatusControl::syncAutoDismissTimer()
  {
    auto const optRemaining = _viewModelPtr ? _viewModelPtr->compactAutoDismissRemaining() : std::nullopt;

    if (!optRemaining)
    {
      cancelAutoDismissTimer();
      return;
    }

    // ActivityStatusFeedProjection::projectNotificationCompact uses kActivityStatusDefaultAutoDismissTimeout
    // for every locally timed Info. A replacement cannot expire sooner, so keep the tick and recheck on delivery.
    // If the projection introduces shorter lifetimes, reschedule for earlier deadlines.
    if (_autoDismissTimerArmed)
    {
      return;
    }

    auto const generation = ++_autoDismissGeneration;
    _autoDismissTickRevoker =
      _autoDismissTimer.Tick(winrt::auto_revoke,
                             [this, generation](winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const&,
                                                winrt::Windows::Foundation::IInspectable const&)
                             {
                               if (generation != _autoDismissGeneration || !_viewModelPtr)
                               {
                                 return;
                               }

                               // Retire the arm before attempting dismissal, which can render synchronously.
                               cancelAutoDismissTimer();

                               if (!_viewModelPtr->tryAutoDismissCompactIfDue())
                               {
                                 syncAutoDismissTimer();
                               }
                             });
    auto const delay = std::chrono::ceil<std::chrono::milliseconds>(*optRemaining);
    _autoDismissTimer.Interval(std::max(delay, std::chrono::milliseconds{1}));
    _autoDismissTimerArmed = true;
    _autoDismissTimer.Start();
  }

  void ActivityStatusControl::cancelAutoDismissTimer() noexcept
  {
    ++_autoDismissGeneration;
    _autoDismissTimerArmed = false;

    if (_autoDismissTimer)
    {
      _autoDismissTimer.Stop();
    }

    _autoDismissTickRevoker.revoke();
  }
} // namespace ao::winui
