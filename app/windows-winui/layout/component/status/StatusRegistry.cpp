// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "status/ActivityStatusControl.h"
#include "track/TrackListController.h"
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/rt/ViewService.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/ShellStatePolicy.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::TextTrimming;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::FontIcon;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::Orientation;
    using winrt::Microsoft::UI::Xaml::Controls::ProgressBar;
    using winrt::Microsoft::UI::Xaml::Controls::ProgressRing;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;

    /// Segoe Fluent "ChromeClose", spelled by code point rather than pasted.
    constexpr auto kDismissGlyph = std::wstring_view{L"\xE711"};

    constexpr double kActivitySpacing = 4.0;
    constexpr double kActivityContentSpacing = 6.0;
    constexpr double kIndicatorSize = 14.0;
    constexpr double kStatusIconFontSize = 16.0;
    constexpr double kLabelFontSize = 11.0;
    constexpr double kLabelMaximumWidth = 220.0;
    constexpr double kProgressWidth = 48.0;
    constexpr double kProgressHeight = 4.0;
    constexpr double kDismissGlyphFontSize = 10.0;
    constexpr double kCompactButtonMinWidth = 0.0;
    constexpr double kCompactButtonHorizontalPadding = 4.0;
    constexpr double kCompactButtonVerticalPadding = 2.0;

    /// The compact buttons in the activity strip carry glyphs, not text.
    Button chromeLessButton()
    {
      auto button = Button{};
      button.MinWidth(kCompactButtonMinWidth);
      button.Padding(Thickness{.Left = kCompactButtonHorizontalPadding,
                               .Top = kCompactButtonVerticalPadding,
                               .Right = kCompactButtonHorizontalPadding,
                               .Bottom = kCompactButtonVerticalPadding});
      return button;
    }

    /**
     * @brief The activity strip: one spinner, label, and progress bar, plus a dismissal.
     *
     * Everything the strip shows is decided by `ActivityStatusControl` from the
     * notification and task services, so the component builds the elements that
     * control drives and hands them over whole.
     */
    class ActivityStatusComponent final : public LayoutComponent
    {
    public:
      explicit ActivityStatusComponent(LayoutBuildContext& ctx)
        : _control{ActivityStatusControlConfig{
            .root = _root,
            .detailButton = _detailButton,
            .spinner = _spinner,
            .statusIcon = _statusIcon,
            .label = _label,
            .progress = _progress,
            .dismissButton = _dismissButton,
            // The strip is the only place notifications can be reached from, so
            // it keeps an affordance even when nothing is running.
            .reserveIdle = true,
          }}
      {
        for (std::int32_t index = 0; index < 2; ++index)
        {
          auto column = ColumnDefinition{};
          column.Width(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
          _root.ColumnDefinitions().Append(column);
        }

        _root.ColumnSpacing(kActivitySpacing);
        _root.VerticalAlignment(VerticalAlignment::Center);
        _root.Visibility(Visibility::Collapsed);

        _spinner.Width(kIndicatorSize);
        _spinner.Height(kIndicatorSize);
        _spinner.IsActive(false);
        _spinner.Visibility(Visibility::Collapsed);

        _statusIcon.FontSize(kStatusIconFontSize);
        _statusIcon.Visibility(Visibility::Collapsed);

        _label.MaxWidth(kLabelMaximumWidth);
        _label.FontSize(kLabelFontSize);
        _label.TextTrimming(TextTrimming::CharacterEllipsis);
        _label.VerticalAlignment(VerticalAlignment::Center);

        _progress.Width(kProgressWidth);
        _progress.Height(kProgressHeight);
        _progress.Minimum(0.0);
        _progress.Maximum(1.0);
        _progress.Visibility(Visibility::Collapsed);
        _progress.VerticalAlignment(VerticalAlignment::Center);

        auto content = StackPanel{};
        content.Orientation(Orientation::Horizontal);
        content.Spacing(kActivityContentSpacing);
        content.VerticalAlignment(VerticalAlignment::Center);
        content.Children().Append(_spinner);
        content.Children().Append(_statusIcon);
        content.Children().Append(_label);
        content.Children().Append(_progress);
        _detailButton.Content(content);

        auto dismissGlyph = FontIcon{};
        dismissGlyph.Glyph(winrt::hstring{kDismissGlyph});
        dismissGlyph.FontSize(kDismissGlyphFontSize);
        _dismissButton.Content(dismissGlyph);
        _dismissButton.Visibility(Visibility::Collapsed);
        ToolTipService::SetToolTip(
          _dismissButton, winrt::box_value(resourceHstring(L"ActivityStatusHideNotificationTooltip")));
        Grid::SetColumn(_dismissButton, 1);

        _root.Children().Append(_detailButton);
        _root.Children().Append(_dismissButton);

        _control.bind(ctx.notifications, ctx.libraryTasks);
      }

      FrameworkElement element() const override { return _root; }

    private:
      Grid _root{};
      Button _detailButton{chromeLessButton()};
      ProgressRing _spinner{};
      FontIcon _statusIcon{};
      TextBlock _label{};
      ProgressBar _progress{};
      Button _dismissButton{chromeLessButton()};
      ActivityStatusControl _control;
    };

    /**
     * @brief Whether @p node reads as part of a browser summary rather than a status bar.
     *
     * A summary yields its space to the filter below the wide tier; a status
     * bar's own content stays put at every width.
     */
    bool isSummaryVariant(uimodel::LayoutNode const& node)
    {
      return node.propertyOr<std::string>("variant", std::string{kStatusVariant}) == kSummaryVariant;
    }

    /// Collapse @p element unless @p state is the width class a summary survives.
    void applySummaryWidth(FrameworkElement const& element, ShellState const& state)
    {
      element.Visibility(state.widthClass == ShellWidthClass::Wide ? Visibility::Visible : Visibility::Collapsed);
    }

    /// How many rows the active list currently holds.
    class TrackCountComponent final : public LayoutComponent
    {
    public:
      TrackCountComponent(LayoutBuildContext& ctx, bool const summary)
        : _trackList{ctx.trackList}, _summary{summary}
      {
        refreshTrackCount();
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(
          ctx.shellStateChanged, "TrackCountComponent", [this](ShellState const state) { applyShellState(state); });
        _trackListChangedSub =
          subscribeUiUpdate(_trackList.signalChanged(), "TrackCountComponent", [this] { refreshTrackCount(); });
      }

      FrameworkElement element() const override { return _text; }

    private:
      void applyShellState(ShellState const& state)
      {
        if (_summary)
        {
          applySummaryWidth(_text, state);
        }
      }

      void refreshTrackCount()
      {
        auto const count = _trackList.rowCount();
        _text.Text(winrt::to_hstring(count == 1 ? resourceString("TrackCountOne")
                                                : formatResource("TrackCountManyFormat", count)));
      }

      TextBlock _text{};
      TrackListController& _trackList;
      bool _summary = false;
      async::Subscription _shellStateSub;
      async::Subscription _trackListChangedSub;
    };

    /**
     * @brief How much of the active list is selected.
     *
     * The selection belongs to the view, not to whichever control the user
     * clicked in, so this reads it from the runtime rather than waiting to be
     * told by a sibling that may not even be in the document.
     */
    class SelectionInfoComponent final : public LayoutComponent
    {
    public:
      SelectionInfoComponent(LayoutBuildContext& ctx, bool const summary)
        : _trackList{ctx.trackList}, _summary{summary}
      {
        follow(ctx.views);
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(
          ctx.shellStateChanged, "SelectionInfoComponent", [this](ShellState const state) { applyShellState(state); });
      }

      FrameworkElement element() const override { return _text; }

    private:
      void applyShellState(ShellState const& state)
      {
        if (_summary)
        {
          applySummaryWidth(_text, state);
        }
      }

      void follow(rt::ViewService& views)
      {
        apply(0);
        _selectionSub = views.onSelectionChanged(
          [this](rt::ViewService::SelectionChanged const& changed) noexcept
          {
            if (changed.viewId == _trackList.viewId())
            {
              apply(changed.selection.size());
            }
          });
      }

      void apply(std::size_t const count)
      {
        auto summary = resourceString("NoSelection");

        if (count == 1)
        {
          summary = resourceString("ItemSelectedOne");
        }
        else if (count > 1)
        {
          summary = formatResource("ItemsSelectedFormat", count);
        }

        _text.Text(winrt::to_hstring(summary));
      }

      TextBlock _text{};
      TrackListController& _trackList;
      bool _summary = false;
      // Destroyed first, so handlers cannot run against a half-torn component.
      async::Subscription _selectionSub;
      async::Subscription _shellStateSub;
    };

    /// The shell's transient message, wherever the document put it.
    class StatusMessageComponent final : public LayoutComponent
    {
    public:
      explicit StatusMessageComponent(LayoutBuildContext& ctx)
      {
        applyMessage(ctx.statusMessage);
        _statusMessageSub = subscribeUiUpdate(
          ctx.statusMessageChanged, "StatusMessageComponent", [this](std::string message) { applyMessage(message); });
      }

      FrameworkElement element() const override { return _text; }

    private:
      void applyMessage(std::string_view const message) { _text.Text(winrt::to_hstring(message)); }

      TextBlock _text{};
      async::Subscription _statusMessageSub;
    };
  } // namespace

  void registerStatusComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      "status.activity",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        // Subscribing to the notification and task services is the one thing a
        // component here does that reports failure by throwing, and a candidate
        // must fail as a value so the live generation survives it.
        try
        {
          return std::make_unique<ActivityStatusComponent>(ctx);
        }
        catch (std::exception const& error)
        {
          return makeError(Error::Code::InitFailed,
                           std::format("Node '{}' could not observe library activity: {}", node.id, error.what()));
        }
      });

    registry.registerComponent(
      "status.trackCount",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<TrackCountComponent>(ctx, isSummaryVariant(node)); });

    registry.registerComponent(
      "status.selectionInfo",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<SelectionInfoComponent>(ctx, isSummaryVariant(node)); });

    registry.registerComponent(
      "status.message",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<StatusMessageComponent>(ctx); });
  }
} // namespace ao::winui::layout
