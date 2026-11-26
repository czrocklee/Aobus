// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackListSelectionEvents.h"
#include "layout/component/track/TrackListView.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "layout/runtime/UiSubscription.h"
#include "platform/StringResourceFormat.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include "track/TrackListSelection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <gsl-lite/gsl-lite.hpp>
#include <winrt/Aobus.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::DataTemplate;
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::SizeChangedEventArgs;
    using winrt::Microsoft::UI::Xaml::Style;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::ItemsControl;
    using winrt::Microsoft::UI::Xaml::Controls::ItemsPanelTemplate;
    using winrt::Microsoft::UI::Xaml::Controls::ItemsStackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::ListView;
    using winrt::Microsoft::UI::Xaml::Controls::ListViewItem;
    using winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem;
    using winrt::Microsoft::UI::Xaml::Controls::RowDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollIntoViewAlignment;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollMode;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollViewer;
    using winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Data::ISelectionInfo;
    using winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs;
    using winrt::Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs;
    using winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper;
    using winrt::Windows::Foundation::IInspectable;

    using ProjectedTrackRowItem = winrt::Aobus::TrackRowItem;

    constexpr auto kHeaderTemplateKey = std::string_view{"TrackHeaderCellTemplate"};
    constexpr auto kRowTemplateKey = std::string_view{"TrackRowTemplate"};
    constexpr auto kRowContainerStyleKey = std::string_view{"TrackListItemStyle"};
    constexpr auto kScrollBarSizeKey = std::string_view{"ScrollBarSize"};

    constexpr double kHeaderRowHeight = 32.0;
    constexpr double kFallbackScrollBarSize = 12.0;
    constexpr double kRecoveryMargin = 12.0;
    constexpr double kRecoverySpacing = 8.0;

    /// The horizontal panel the header strip lays its cells out in.
    constexpr auto kHeaderPanelMarkup =
      std::string_view{R"(<ItemsPanelTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">)"
                       R"(<StackPanel Orientation="Horizontal" /></ItemsPanelTemplate>)"};

    /// The row that owns @p source, found by walking out to the nearest data context.
    ProjectedTrackRowItem trackRowFromEventSource(IInspectable const& source)
    {
      auto current = source.try_as<winrt::Microsoft::UI::Xaml::DependencyObject>();

      while (current)
      {
        if (auto const element = current.try_as<FrameworkElement>(); element)
        {
          if (auto const row = element.DataContext().try_as<ProjectedTrackRowItem>(); row)
          {
            return row;
          }
        }

        current = VisualTreeHelper::GetParent(current);
      }

      return nullptr;
    }

    /**
     * @brief The scrolling track surface: one header strip above one row list.
     *
     * Column geometry is solved by `TrackListController` from the viewport it is
     * told about, so the component reports its own width and applies the solved
     * content width back onto the surface. Cell appearance stays in the compiled
     * templates the window frame owns.
     */
    class TrackTableComponent final : public LayoutComponent
    {
    public:
      TrackTableComponent(LayoutBuildContext& ctx,
                          TrackListController& trackList,
                          std::function<Result<>(rt::ViewId, TrackId)> playTrack,
                          std::function<std::vector<uimodel::WritableTagListTarget>()> membershipTargets,
                          std::function<void(ListId, bool)> editMembership,
                          std::function<uimodel::ListOrderCapabilityState()> orderCapabilities,
                          std::function<void(ListOrderCommand)> applyOrder,
                          ActionRegistry const& actions,
                          std::function<std::string(std::string_view)> shortcutHint,
                          i18n::MessageCatalog textCatalog,
                          std::function<void(std::string)> reportStatus,
                          std::function<void()> chooseColumns,
                          DataTemplate const& headerTemplate,
                          DataTemplate const& rowTemplate,
                          Style const& rowContainerStyle,
                          double const trailingChromeWidth)
        : _trackList{trackList}
        , _playTrack{std::move(playTrack)}
        , _actions{actions}
        , _shortcutHint{std::move(shortcutHint)}
        , _membershipTargets{std::move(membershipTargets)}
        , _editMembership{std::move(editMembership)}
        , _orderCapabilities{std::move(orderCapabilities)}
        , _applyOrder{std::move(applyOrder)}
        , _textCatalog{std::move(textCatalog)}
        , _gatePtr{ctx.gatePtr}
        , _reportStatus{std::move(reportStatus)}
        , _trailingChromeWidth{trailingChromeWidth}
      {
        _headers.ItemTemplate(headerTemplate);
        _headers.ItemsPanel(winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::to_hstring(kHeaderPanelMarkup))
                              .as<ItemsPanelTemplate>());
        _headers.BorderThickness(Thickness{.Left = 0.0, .Top = 0.0, .Right = 0.0, .Bottom = 1.0});
        _headers.Height(kHeaderRowHeight);

        auto message = TextBlock{};
        message.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiTrackNoVisibleColumns)));
        message.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
        _columnsRecovery.Margin(Thickness{
          .Left = kRecoveryMargin, .Top = kRecoveryMargin, .Right = kRecoveryMargin, .Bottom = kRecoveryMargin});
        _columnsRecovery.Spacing(kRecoverySpacing);
        _columnsRecovery.Children().Append(message);

        if (chooseColumns)
        {
          _chooseColumns.Content(
            winrt::box_value(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiShellColumns))));
          _chooseColumnsRevoker = _chooseColumns.Click(
            winrt::auto_revoke,
            [gatePtr = _gatePtr, chooseColumns = std::move(chooseColumns)](auto const&, auto const&)
            {
              if (uimodel::isGenerationActive(gatePtr))
              {
                chooseColumns();
              }
            });
          _columnsRecovery.Children().Append(_chooseColumns);
        }

        _rows.ItemTemplate(rowTemplate);
        _rows.SelectionMode(ListViewSelectionMode::Extended);
        ScrollViewer::SetHorizontalScrollBarVisibility(_rows, ScrollBarVisibility::Disabled);
        ScrollViewer::SetHorizontalScrollMode(_rows, ScrollMode::Disabled);
        _selectionChangedRevoker =
          _rows.SelectionChanged(winrt::auto_revoke, {this, &TrackTableComponent::onSelectionChanged});
        _doubleTappedRevoker = _rows.DoubleTapped(winrt::auto_revoke, {this, &TrackTableComponent::onDoubleTapped});
        _rightTappedRevoker = _rows.RightTapped(winrt::auto_revoke, {this, &TrackTableComponent::onRightTapped});

        if (rowContainerStyle)
        {
          _rows.ItemContainerStyle(rowContainerStyle);
        }

        auto headerRow = RowDefinition{};
        headerRow.Height(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Auto});
        auto rowsRow = RowDefinition{};
        rowsRow.Height(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
        _surface.RowDefinitions().Append(headerRow);
        _surface.RowDefinitions().Append(rowsRow);
        Grid::SetRow(_rows, 1);
        _surface.Children().Append(_headers);
        _surface.Children().Append(_columnsRecovery);
        _surface.Children().Append(_rows);

        // The surface is wider than the viewport whenever the solved columns are,
        // so only the viewport scrolls and the rows never scroll horizontally.
        _viewport.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
        _viewport.HorizontalScrollMode(ScrollMode::Enabled);
        _viewport.VerticalScrollBarVisibility(ScrollBarVisibility::Disabled);
        _viewport.VerticalScrollMode(ScrollMode::Disabled);
        _viewport.Content(_surface);
        _sizeChangedRevoker =
          _viewport.SizeChanged(winrt::auto_revoke, {this, &TrackTableComponent::onViewportSizeChanged});

        _headers.ItemsSource(_trackList.headers());
        refreshTrackList();
        _trackListChangedSub =
          subscribeUiUpdate(_trackList.signalChanged(), "TrackTableComponent", [this] { refreshTrackList(); });
      }

      ~TrackTableComponent() override { dismissContextMenu(); }

      TrackTableComponent(TrackTableComponent const&) = delete;
      TrackTableComponent& operator=(TrackTableComponent const&) = delete;
      TrackTableComponent(TrackTableComponent&&) = delete;
      TrackTableComponent& operator=(TrackTableComponent&&) = delete;

      FrameworkElement element() const override { return _viewport; }

    private:
      std::vector<std::uint32_t> selectedIndices(std::span<TrackId const> selection) const
      {
        auto indices = std::vector<std::uint32_t>{};
        indices.reserve(selection.size());

        for (auto const id : selection)
        {
          if (auto const optIndex = _trackList.displayIndexOfTrack(id); optIndex)
          {
            indices.push_back(static_cast<std::uint32_t>(*optIndex));
          }
        }

        return indices;
      }

      enum class RowBinding : std::uint8_t
      {
        Stale,
        Updating,
        UpdateRequested,
        Ready
      };

      bool acceptsRowInput() const
      {
        return _rowBinding == RowBinding::Ready && uimodel::isGenerationActive(_gatePtr) &&
               _trackList.isCurrentRowSource(_rows.ItemsSource());
      }

      bool acceptsTrackRow(ProjectedTrackRowItem const& row) const
      {
        std::uint32_t index = 0;
        return acceptsRowInput() && row && !row.IsGroupHeader() && row.TrackId() != 0 &&
               _trackList.items().IndexOf(row, index);
      }

      ProjectedTrackRowItem trackRowForGesture(IInspectable const& source) const
      {
        if (!uimodel::isGenerationActive(_gatePtr))
        {
          return nullptr;
        }

        auto row = trackRowFromEventSource(source);
        return acceptsTrackRow(row) ? row : nullptr;
      }

      void refreshTrackList()
      {
        _selectionEvents.interrupt();

        if (_rowBinding == RowBinding::Updating || _rowBinding == RowBinding::UpdateRequested)
        {
          // A native setter can call back into the controller. Never restore two
          // selections onto the same ListView concurrently on the dispatcher.
          _rowBinding = RowBinding::UpdateRequested;
          return;
        }

        {
          auto retireIncomplete = gsl_lite::finally(
            [this]
            {
              if (_rowBinding != RowBinding::Ready)
              {
                _rowBinding = RowBinding::Stale;
              }
            });
          _rowBinding = RowBinding::UpdateRequested;

          while (_rowBinding == RowBinding::UpdateRequested)
          {
            _rowBinding = RowBinding::Updating;
            bindTrackRows();
          }

          _rowBinding = RowBinding::Ready;
        }

        if (auto const source = _rows.ItemsSource().try_as<ISelectionInfo>(); source)
        {
          _selectionEvents.commit(_rows, source.GetSelectedRanges(), _gatePtr);
        }
      }

      void dismissContextMenu()
      {
        if (auto flyout = std::exchange(_contextFlyout, nullptr); flyout)
        {
          _contextClickRevokers.clear();
          flyout.Hide();
        }
      }

      void bindTrackRows()
      {
        dismissContextMenu();
        auto const items = _trackList.items();
        auto optTopIndex = std::optional<std::size_t>{};
        auto topTrackId = kInvalidTrackId;
        auto const selection = _trackList.selection();

        if (auto const panel = _rows.ItemsPanelRoot().try_as<ItemsStackPanel>(); panel)
        {
          // At the top without a selection, show the new first row. Else anchor to
          // the realized row, not a lazy read against an already changed projection.
          auto const firstVisible = panel.FirstVisibleIndex();

          if (firstVisible >= 0)
          {
            optTopIndex = static_cast<std::size_t>(firstVisible);
          }

          if (firstVisible > 0 || (firstVisible == 0 && !selection.empty()))
          {
            auto const container = _rows.ContainerFromIndex(firstVisible).try_as<ListViewItem>();

            if (auto const row = container ? container.Content().try_as<ProjectedTrackRowItem>() : nullptr;
                row && !row.IsGroupHeader() && row.TrackId() != 0)
            {
              topTrackId = TrackId{row.TrackId()};
            }
          }
        }

        _rows.ItemsSource(items);

        if (_rowBinding == RowBinding::UpdateRequested)
        {
          return;
        }

        restoreTrackSelection(_rows, selectedIndices(selection));

        auto const headers = _trackList.headers();
        auto const showRecovery = _trackList.rowCount() != 0 && headers.Size() == 0;
        _headers.ItemsSource(headers);
        _headers.Visibility(showRecovery ? Visibility::Collapsed : Visibility::Visible);
        _columnsRecovery.Visibility(showRecovery ? Visibility::Visible : Visibility::Collapsed);
        _surface.Width(_trackList.contentWidth());

        if (_rowBinding == RowBinding::UpdateRequested)
        {
          return;
        }

        if (optTopIndex)
        {
          auto optRestoreIndex = optTopIndex;

          if (topTrackId != kInvalidTrackId)
          {
            if (auto const optMappedIndex = _trackList.displayIndexOfTrack(topTrackId); optMappedIndex)
            {
              optRestoreIndex = optMappedIndex;
            }
          }

          if (optRestoreIndex && *optRestoreIndex < items.Size())
          {
            _rows.ScrollIntoView(
              items.GetAt(static_cast<std::uint32_t>(*optRestoreIndex)), ScrollIntoViewAlignment::Leading);
          }
        }

        auto const optReveal = _trackList.revealTarget();

        if (_rowBinding == RowBinding::UpdateRequested || !optReveal || optReveal->serial == _lastRevealSerial ||
            optReveal->displayIndex >= items.Size())
        {
          return;
        }

        _rows.ScrollIntoView(items.GetAt(static_cast<std::uint32_t>(optReveal->displayIndex)));
        _lastRevealSerial = optReveal->serial;
      }

      void onViewportSizeChanged(IInspectable const& /*sender*/, SizeChangedEventArgs const& args)
      {
        if (args.NewSize().Width <= 0.0 || !uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        auto const revision = _selectionEvents.revision();
        _trackList.setViewportWidth(args.NewSize().Width, _trailingChromeWidth);

        if (revision.isAlive() && uimodel::isGenerationActive(_gatePtr))
        {
          _surface.Width(_trackList.contentWidth());
        }
      }

      void onSelectionChanged(IInspectable const& /*sender*/, SelectionChangedEventArgs const& /*args*/)
      {
        if (!acceptsRowInput())
        {
          return;
        }

        using winrt::Microsoft::UI::Xaml::Data::ItemIndexRange;
        auto const items = _trackList.items();
        auto const revision = _selectionEvents.revision();
        auto const source = items.as<ISelectionInfo>();
        auto ranges = source.GetSelectedRanges();
        auto const previous = _trackList.selection();
        auto displayOrder = std::vector<TrackId>{};
        auto headers = std::vector<std::int32_t>{};

        for (auto const& range : ranges)
        {
          auto const start = static_cast<std::uint32_t>(range.FirstIndex());
          auto const length = range.Length();

          for (std::uint32_t offset = 0; offset < length; ++offset)
          {
            auto const index = start + offset;

            if (auto const id = _trackList.trackIdAtDisplayIndex(index); id != kInvalidTrackId)
            {
              displayOrder.push_back(id);
            }
            else
            {
              headers.push_back(static_cast<std::int32_t>(index));
            }
          }
        }

        if (!headers.empty())
        {
          // ListView is still inside its selection transaction and ignores nested
          // control gestures. Correct the source it consults for the final state.
          for (auto const index : headers)
          {
            source.DeselectRange(ItemIndexRange{index, 1});
          }

          if (displayOrder.empty())
          {
            // A header-only gesture leaves even filtered-out runtime IDs intact.
            for (auto const& range : trackSelectionRanges(selectedIndices(previous)))
            {
              source.SelectRange(range);
            }

            _selectionEvents.commit(_rows, source.GetSelectedRanges(), _gatePtr);
            return;
          }

          ranges = source.GetSelectedRanges();
        }

        // Ranges arrive in display order; keep the user's first selected track
        // first (including across reorders), then append newly selected tracks.
        auto remaining = std::unordered_set<TrackId>{displayOrder.begin(), displayOrder.end()};
        auto selected = std::vector<TrackId>{};
        selected.reserve(displayOrder.size());

        for (auto const id : previous)
        {
          if (remaining.erase(id) != 0)
          {
            selected.push_back(id);
          }
        }

        for (auto const id : displayOrder)
        {
          if (remaining.erase(id) != 0)
          {
            selected.push_back(id);
          }
        }

        if (selected != previous && _trackList.isCurrentRowSource(items) && acceptsRowInput())
        {
          dismissContextMenu();

          if (!revision.isCurrent() || !acceptsRowInput())
          {
            return;
          }

          _trackList.publishSelection(selected);
        }

        // Publishing to Runtime can synchronously rebind the table. Do not announce the old choice.
        if (revision.isCurrent() && acceptsRowInput() && _trackList.isCurrentRowSource(items))
        {
          _selectionEvents.commit(_rows, ranges, _gatePtr);
        }
      }

      void onDoubleTapped(IInspectable const& /*sender*/, DoubleTappedRoutedEventArgs const& args)
      {
        auto const row = trackRowForGesture(args.OriginalSource());

        if (!row)
        {
          return;
        }

        auto const playedRes = _trackList.play(TrackId{row.TrackId()}, _playTrack);

        if (!playedRes && _reportStatus)
        {
          _reportStatus(formatResource("playback_failed", playedRes.error().message));
        }
      }

      // The context menu is one ordered surface whose branching mirrors its
      // visible sections; keeping the assembly together preserves that order.
      // NOLINTNEXTLINE(readability-function-cognitive-complexity)
      void onRightTapped(IInspectable const& /*sender*/, RightTappedRoutedEventArgs const& args)
      {
        auto const row = trackRowForGesture(args.OriginalSource());

        if (!row)
        {
          return;
        }

        if (auto const trackId = TrackId{row.TrackId()}; !std::ranges::contains(_trackList.selection(), trackId))
        {
          auto const revision = _selectionEvents.revision();
          _rows.SelectedItem(row);

          if (!revision.isAlive() || !acceptsTrackRow(row) || !std::ranges::contains(_trackList.selection(), trackId))
          {
            return;
          }
        }

        dismissContextMenu();
        _contextFlyout = MenuFlyout{};
        auto const appendItem = [this, &row](auto const& items,
                                             std::string_view const text,
                                             std::function<void()> callback,
                                             bool const enabled = true,
                                             std::string_view const accelerator = {})
        {
          auto item = MenuFlyoutItem{};
          item.Text(winrt::to_hstring(text));
          item.IsEnabled(enabled);

          if (!accelerator.empty())
          {
            item.KeyboardAcceleratorTextOverride(winrt::to_hstring(accelerator));
          }

          if (callback)
          {
            _contextClickRevokers.push_back(
              item.Click(winrt::auto_revoke,
                         [this, row, callback = std::move(callback)](
                           IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                         {
                           if (acceptsTrackRow(row))
                           {
                             callback();
                           }
                         }));
          }

          items.Append(item);
        };

        auto properties = MenuFlyoutItem{};
        properties.Text(winrt::to_hstring(resourceString("winui_track_properties_command")));
        properties.KeyboardAcceleratorTextOverride(L"Alt+Enter");
        _contextClickRevokers.push_back(
          properties.Click(winrt::auto_revoke,
                           [this, row](IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                           {
                             if (acceptsTrackRow(row))
                             {
                               std::ignore = _actions.tryInvoke("track.presentProperties", ActionContext{});
                             }
                           }));
        _contextFlyout.Items().Append(properties);

        if (_membershipTargets && _editMembership)
        {
          auto const membership =
            uimodel::listMembershipMenu(_textCatalog, _membershipTargets(), _trackList.activeListId());
          auto addSubmenu = MenuFlyoutSubItem{};
          addSubmenu.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListAddToPlaylist)));

          for (auto const& target : membership.addTargets)
          {
            appendItem(addSubmenu.Items(),
                       target.label,
                       [edit = _editMembership, listId = target.listId] { edit(listId, true); });
          }

          if (membership.addTargets.empty())
          {
            appendItem(addSubmenu.Items(),
                       i18n::requiredText(_textCatalog, i18n::MessageId::ListNoMembershipAddTargets),
                       {},
                       false);
          }

          _contextFlyout.Items().Append(addSubmenu);

          if (membership.optRemoveFromCurrent)
          {
            appendItem(_contextFlyout.Items(),
                       membership.optRemoveFromCurrent->label,
                       [edit = _editMembership, listId = membership.optRemoveFromCurrent->listId]
                       { edit(listId, false); });
          }
        }

        if (_orderCapabilities && _applyOrder)
        {
          _contextFlyout.Items().Append(MenuFlyoutSeparator{});
          auto ordering = MenuFlyoutSubItem{};
          ordering.Text(winrt::to_hstring(i18n::requiredText(_textCatalog, i18n::MessageId::ListManualOrder)));

          if (auto const capabilities = _orderCapabilities(); !capabilities.canAuthorOrder)
          {
            appendItem(ordering.Items(), capabilities.disabledReason, {}, false);
          }
          else
          {
            auto const action = [this](std::string_view const id)
            { return [this, id = std::string{id}] { std::ignore = _actions.tryInvoke(id, ActionContext{}); }; };
            appendItem(ordering.Items(),
                       i18n::requiredText(_textCatalog, i18n::MessageId::ListMoveUp),
                       action("track.orderMoveUp"),
                       capabilities.canRelativeMove,
                       _shortcutHint("track.orderMoveUp"));
            appendItem(ordering.Items(),
                       i18n::requiredText(_textCatalog, i18n::MessageId::ListMoveDown),
                       action("track.orderMoveDown"),
                       capabilities.canRelativeMove,
                       _shortcutHint("track.orderMoveDown"));
            appendItem(ordering.Items(),
                       i18n::requiredText(_textCatalog, i18n::MessageId::ListMoveToTop),
                       action("track.orderMoveToTop"),
                       capabilities.canAbsoluteMove,
                       _shortcutHint("track.orderMoveToTop"));
            appendItem(ordering.Items(),
                       i18n::requiredText(_textCatalog, i18n::MessageId::ListMoveToBottom),
                       action("track.orderMoveToBottom"),
                       capabilities.canAbsoluteMove,
                       _shortcutHint("track.orderMoveToBottom"));
            appendItem(
              ordering.Items(),
              i18n::requiredText(_textCatalog, i18n::MessageId::ListResetOrder),
              [apply = _applyOrder] { apply(ListOrderCommand::Reset); },
              capabilities.canResetOrder);
          }

          _contextFlyout.Items().Append(ordering);
        }

        auto const anchor = _rows.ContainerFromItem(row).try_as<FrameworkElement>();
        _contextFlyout.ShowAt(anchor ? anchor : _rows);
        args.Handled(true);
      }

      ScrollViewer _viewport{};
      Grid _surface{};
      ItemsControl _headers{};
      StackPanel _columnsRecovery{};
      Button _chooseColumns{};
      ListView _rows = makeTrackListView();
      TrackListController& _trackList;
      std::function<Result<>(rt::ViewId, TrackId)> _playTrack;
      ActionRegistry const& _actions;
      std::function<std::string(std::string_view)> _shortcutHint;
      std::function<std::vector<uimodel::WritableTagListTarget>()> _membershipTargets;
      std::function<void(ListId, bool)> _editMembership;
      std::function<uimodel::ListOrderCapabilityState()> _orderCapabilities;
      std::function<void(ListOrderCommand)> _applyOrder;
      i18n::MessageCatalog _textCatalog;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::function<void(std::string)> _reportStatus;
      double _trailingChromeWidth = kFallbackScrollBarSize;
      std::uint64_t _lastRevealSerial = 0;
      RowBinding _rowBinding = RowBinding::Stale;
      TrackListSelectionEvents _selectionEvents;
      async::Subscription _trackListChangedSub;
      ListView::SelectionChanged_revoker _selectionChangedRevoker{};
      ListView::DoubleTapped_revoker _doubleTappedRevoker{};
      ListView::RightTapped_revoker _rightTappedRevoker{};
      Button::Click_revoker _chooseColumnsRevoker{};
      FrameworkElement::SizeChanged_revoker _sizeChangedRevoker{};
      MenuFlyout _contextFlyout{nullptr};
      std::vector<MenuFlyoutItem::Click_revoker> _contextClickRevokers;
    };
  } // namespace

  void registerTrackTableComponent(ComponentRegistry& registry,
                                   TrackListController& trackList,
                                   std::function<Result<>(rt::ViewId, TrackId)> playTrack,
                                   std::function<std::vector<uimodel::WritableTagListTarget>()> membershipTargets,
                                   std::function<void(ListId, bool)> editMembership,
                                   std::function<uimodel::ListOrderCapabilityState()> orderCapabilities,
                                   std::function<void(ListOrderCommand)> applyOrder,
                                   ActionRegistry const& actions,
                                   std::function<std::string(std::string_view)> shortcutHint,
                                   i18n::MessageCatalog textCatalog,
                                   std::function<void(std::string)> reportStatus,
                                   std::function<void()> chooseColumns)
  {
    registry.registerComponent(
      "track.table",
      [&trackList,
       playTrack = std::move(playTrack),
       membershipTargets = std::move(membershipTargets),
       editMembership = std::move(editMembership),
       orderCapabilities = std::move(orderCapabilities),
       applyOrder = std::move(applyOrder),
       &actions,
       shortcutHint = std::move(shortcutHint),
       textCatalog = std::move(textCatalog),
       reportStatus = std::move(reportStatus),
       chooseColumns = std::move(chooseColumns)](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        auto const headerTemplate = lookupResource(ctx.resources, kHeaderTemplateKey).try_as<DataTemplate>();
        auto const rowTemplate = lookupResource(ctx.resources, kRowTemplateKey).try_as<DataTemplate>();

        if (!headerTemplate || !rowTemplate)
        {
          return makeError(Error::Code::NotFound,
                           std::format("Node '{}' needs the '{}' and '{}' item templates, which the window frame "
                                       "does not define",
                                       node.id,
                                       kHeaderTemplateKey,
                                       kRowTemplateKey));
        }

        auto const scrollBarSize = lookupResource(ctx.resources, kScrollBarSizeKey);

        return std::make_unique<TrackTableComponent>(
          ctx,
          trackList,
          playTrack,
          membershipTargets,
          editMembership,
          orderCapabilities,
          applyOrder,
          actions,
          shortcutHint,
          textCatalog,
          reportStatus,
          chooseColumns,
          headerTemplate,
          rowTemplate,
          lookupResource(ctx.resources, kRowContainerStyleKey).try_as<Style>(),
          winrt::unbox_value_or<double>(scrollBarSize, kFallbackScrollBarSize));
      });
  }
} // namespace ao::winui::layout
