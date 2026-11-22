// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ComponentRegistrations.h"

// Asio's Win32 headers define GetCurrentTime(); XAML declares a method with that name.
#undef GetCurrentTime

#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "layout/runtime/UiSubscription.h"
#include "platform/NamedControl.h"
#include "platform/WinRtStringResources.h"
#include "status/ActivityStatusControl.h"
#include "track/TrackListController.h"
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/track/TrackSelectionSummary.h>
#include <ao/winui/layout/LayoutSchema.h>
#include <ao/winui/layout/ShellState.h>

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::ResourceDictionary;
    using winrt::Microsoft::UI::Xaml::Style;
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

    /// The compact buttons in the activity strip carry glyphs, not text, so
    /// @p name is their only accessible label.
    Button chromeLessButton(winrt::hstring const& name)
    {
      auto button = makeNamed<Button>(name);
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
      ActivityStatusComponent(ResourceDictionary const& resources,
                              rt::NotificationService& notifications,
                              rt::LibraryJobs& libraryJobs,
                              i18n::MessageCatalog const& textCatalog)
      {
        for (std::int32_t index = 0; index < 2; ++index)
        {
          auto column = ColumnDefinition{};
          column.Width(GridLength{.Value = 0.0, .GridUnitType = GridUnitType::Auto});
          _root.ColumnDefinitions().Append(column);
        }

        _root.ColumnSpacing(kActivitySpacing);
        _root.VerticalAlignment(VerticalAlignment::Center);

        _spinner.Width(kIndicatorSize);
        _spinner.Height(kIndicatorSize);

        _statusIcon.FontSize(kStatusIconFontSize);
        // Only the action is unavailable; its information must retain normal,
        // theme-bound contrast instead of inheriting the Button's disabled ink.
        _statusIcon.Style(lookupResource(resources, "ActivityStatusIconStyle").try_as<Style>());
        _label.Style(lookupResource(resources, "ActivityStatusTextStyle").try_as<Style>());

        _label.MaxWidth(kLabelMaximumWidth);
        _label.FontSize(kLabelFontSize);
        _label.TextTrimming(TextTrimming::CharacterEllipsis);
        _label.VerticalAlignment(VerticalAlignment::Center);

        _progress.Width(kProgressWidth);
        _progress.Height(kProgressHeight);
        _progress.Minimum(0.0);
        _progress.Maximum(1.0);
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
        auto const hideLabel = resourceHstring(L"winui_activity_hide_notification");
        ToolTipService::SetToolTip(_dismissButton, winrt::box_value(hideLabel));
        Grid::SetColumn(_dismissButton, 1);

        _root.Children().Append(_detailButton);
        _root.Children().Append(_dismissButton);

        // Construct the observer only after the elements are configured: it
        // renders the current state synchronously.
        _optControl.emplace(
          ActivityStatusControlConfig{
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
            .textCatalog = textCatalog,
          },
          notifications,
          libraryJobs);
      }

      FrameworkElement element() const override { return _root; }

    private:
      Grid _root{};
      Button _detailButton{chromeLessButton(resourceHstring(L"winui_activity_notifications"))};
      ProgressRing _spinner{};
      FontIcon _statusIcon{};
      TextBlock _label{};
      ProgressBar _progress{};
      Button _dismissButton{chromeLessButton(resourceHstring(L"winui_activity_hide_notification"))};
      std::optional<ActivityStatusControl> _optControl;
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
      TrackCountComponent(LayoutBuildContext& ctx,
                          TrackListController& trackList,
                          i18n::MessageCatalog textCatalog,
                          async::Signal<ShellState>& shellStateChanged,
                          bool const summary)
        : _trackList{trackList}, _textCatalog{std::move(textCatalog)}, _summary{summary}
      {
        refreshTrackCount();
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(
          shellStateChanged, "TrackCountComponent", [this](ShellState const state) { applyShellState(state); });
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
        _text.Text(
          winrt::to_hstring(i18n::requiredFormat(_textCatalog, i18n::MessageId::TrackCount, {{"count", count}})));
      }

      TextBlock _text{};
      TrackListController& _trackList;
      i18n::MessageCatalog _textCatalog;
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
      SelectionInfoComponent(LayoutBuildContext& ctx,
                             TrackListController& trackList,
                             rt::ViewService& views,
                             rt::LibraryChanges const& changes,
                             i18n::MessageCatalog textCatalog,
                             async::Signal<ShellState>& shellStateChanged,
                             bool const summary)
        : _trackList{trackList}
        , _views{views}
        , _textCatalog{std::move(textCatalog)}
        , _gatePtr{ctx.gatePtr}
        , _viewId{trackList.viewId()}
        , _summary{summary}
      {
        // Render the live selection even while this shell is still staged.
        apply(_viewId, _views.selectedTrackIds(_viewId).size());
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(
          shellStateChanged, "SelectionInfoComponent", [this](ShellState const state) { applyShellState(state); });
        _trackListChangedSub =
          subscribeUiUpdate(_trackList.signalChanged(), "SelectionInfoComponent", [this] { refreshActiveView(); });
        _librarySub = changes.onChanged(
          [this](rt::LibraryChangeSet const& changed)
          {
            if (!uimodel::isGenerationActive(_gatePtr))
            {
              return;
            }

            auto const viewId = _trackList.viewId();
            auto const selected = _views.selectedTrackIds(viewId);

            if (uimodel::shouldRefreshTrackSelectionSummary(changed, selected))
            {
              applyUiUpdate(
                "SelectionInfoComponent", [this, viewId, count = selected.size()] { apply(viewId, count); });
            }
          });
        _selectionSub = _views.onSelectionChanged(
          [this](rt::ViewService::SelectionChanged const& changed)
          {
            if (changed.viewId == _trackList.viewId() && uimodel::isGenerationActive(_gatePtr))
            {
              applyUiUpdate(
                "SelectionInfoComponent", [this, &changed] { apply(changed.viewId, changed.selection.size()); });
            }
          });
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

      void refreshActiveView()
      {
        // Projection and viewport changes also use this signal; only a view
        // switch changes which selection should be summarized.
        if (auto const viewId = _trackList.viewId(); viewId != _viewId && uimodel::isGenerationActive(_gatePtr))
        {
          _viewId = viewId;
          apply(viewId, _views.selectedTrackIds(viewId).size());
        }
      }

      void apply(rt::ViewId const viewId, std::size_t const count)
      {
        auto const duration = _views.selectionDuration(viewId);
        _text.Text(winrt::to_hstring(uimodel::trackSelectionSummaryText(_textCatalog, count, duration)));
      }

      TextBlock _text{};
      TrackListController& _trackList;
      rt::ViewService& _views;
      i18n::MessageCatalog _textCatalog;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      rt::ViewId _viewId;
      bool _summary = false;
      // Destroyed first, so handlers cannot run against a half-torn component.
      async::Subscription _shellStateSub;
      async::Subscription _trackListChangedSub;
      async::Subscription _selectionSub;
      async::Subscription _librarySub;
    };

    /**
     * @brief The shell's transient message, wherever the document put it.
     *
     * A single line can be narrower than the message, and in a shell without an
     * activity surface this line is the message's only presentation. It trims
     * with an ellipsis so the cut is visible, and its tooltip carries the full text.
     */
    class StatusMessageComponent final : public LayoutComponent
    {
    public:
      StatusMessageComponent(LayoutBuildContext& ctx, async::Signal<std::string>& statusMessageChanged)
      {
        _text.TextTrimming(TextTrimming::CharacterEllipsis);
        applyMessage(ctx.statusMessage);
        _statusMessageSub = subscribeUiUpdate(
          statusMessageChanged, "StatusMessageComponent", [this](std::string message) { applyMessage(message); });
      }

      FrameworkElement element() const override { return _text; }

    private:
      void applyMessage(std::string_view const message)
      {
        auto const text = winrt::to_hstring(message);
        _text.Text(text);
        ToolTipService::SetToolTip(
          _text, text.empty() ? winrt::Windows::Foundation::IInspectable{nullptr} : winrt::box_value(text));
      }

      TextBlock _text{};
      async::Subscription _statusMessageSub;
    };
  } // namespace

  void registerStatusComponents(ComponentRegistry& registry,
                                rt::ViewService& views,
                                rt::NotificationService& notifications,
                                rt::Library& library,
                                TrackListController& trackList,
                                i18n::MessageCatalog textCatalog,
                                async::Signal<ShellState>& shellStateChanged,
                                async::Signal<std::string>& statusMessageChanged)
  {
    registry.registerComponent(
      "status.activity",
      [&notifications, &libraryJobs = library.jobs(), textCatalog](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<ActivityStatusComponent>(ctx.resources, notifications, libraryJobs, textCatalog); });

    registry.registerComponent(
      "status.trackCount",
      [&trackList, textCatalog, &shellStateChanged](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<TrackCountComponent>(
          ctx, trackList, textCatalog, shellStateChanged, isSummaryVariant(node));
      });

    registry.registerComponent(
      "status.selectionInfo",
      [&trackList, &views, &changes = library.changes(), textCatalog, &shellStateChanged](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<SelectionInfoComponent>(
          ctx, trackList, views, changes, textCatalog, shellStateChanged, isSummaryVariant(node));
      });

    registry.registerComponent(
      "status.message",
      [&statusMessageChanged](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<StatusMessageComponent>(ctx, statusMessageChanged); });
  }
} // namespace ao::winui::layout
