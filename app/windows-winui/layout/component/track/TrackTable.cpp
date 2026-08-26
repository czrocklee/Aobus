// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackTable.h"

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <gsl-lite/gsl-lite.hpp>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
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
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::ItemsControl;
    using winrt::Microsoft::UI::Xaml::Controls::ItemsPanelTemplate;
    using winrt::Microsoft::UI::Xaml::Controls::ItemsStackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::ListView;
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

    std::string tagExpression(std::string_view const tag)
    {
      return query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = std::string{tag}});
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
                          DataTemplate const& headerTemplate,
                          DataTemplate const& rowTemplate,
                          Style const& rowContainerStyle,
                          double const trailingChromeWidth)
        : _trackList{ctx.trackList}
        , _playTrack{ctx.library.playTrack}
        , _actions{ctx.actions}
        , _membershipTargets{ctx.library.membershipTargets}
        , _editMembership{ctx.library.editMembership}
        , _orderCapabilities{ctx.library.orderCapabilities}
        , _applyOrder{ctx.library.applyOrder}
        , _textCatalog{ctx.textCatalog}
        , _gatePtr{ctx.gatePtr}
        , _reportStatus{ctx.reportStatus}
        , _trailingChromeWidth{trailingChromeWidth}
      {
        _headers.ItemTemplate(headerTemplate);
        _headers.ItemsPanel(winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(winrt::to_hstring(kHeaderPanelMarkup))
                              .as<ItemsPanelTemplate>());
        _headers.BorderThickness(Thickness{.Left = 0.0, .Top = 0.0, .Right = 0.0, .Bottom = 1.0});

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
        headerRow.Height(GridLength{.Value = kHeaderRowHeight, .GridUnitType = GridUnitType::Pixel});
        auto rowsRow = RowDefinition{};
        rowsRow.Height(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
        _surface.RowDefinitions().Append(headerRow);
        _surface.RowDefinitions().Append(rowsRow);
        Grid::SetRow(_rows, 1);
        _surface.Children().Append(_headers);
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

      ~TrackTableComponent() override
      {
        if (_contextFlyout)
        {
          _contextClickRevokers.clear();
          _contextFlyout.Hide();
        }
      }

      TrackTableComponent(TrackTableComponent const&) = delete;
      TrackTableComponent& operator=(TrackTableComponent const&) = delete;
      TrackTableComponent(TrackTableComponent&&) = delete;
      TrackTableComponent& operator=(TrackTableComponent&&) = delete;

      FrameworkElement element() const override { return _viewport; }

    private:
      void refreshTrackList()
      {
        auto optTopIndex = std::optional<std::size_t>{};
        auto topTrackId = kInvalidTrackId;
        auto const oldSource = _rows.ItemsSource();
        auto const oldItems =
          oldSource ? oldSource.try_as<winrt::Windows::Foundation::Collections::IVectorView<IInspectable>>() : nullptr;

        if (auto const panel = _rows.ItemsPanelRoot().try_as<ItemsStackPanel>(); panel)
        {
          if (auto const firstVisible = panel.FirstVisibleIndex();
              firstVisible >= 0 && oldItems && static_cast<std::uint32_t>(firstVisible) < oldItems.Size())
          {
            optTopIndex = static_cast<std::size_t>(firstVisible);

            if (auto const row =
                  oldItems.GetAt(static_cast<std::uint32_t>(firstVisible)).try_as<ProjectedTrackRowItem>();
                row && !row.IsGroupHeader() && row.TrackId() != 0)
            {
              topTrackId = TrackId{row.TrackId()};
            }
          }
        }

        auto const previousSuppression = _suppressSelectionPublication;
        _suppressSelectionPublication = true;
        auto restoreSuppression =
          gsl_lite::finally([this, previousSuppression] { _suppressSelectionPublication = previousSuppression; });
        auto const items = _trackList.items();
        _rows.ItemsSource(items);
        _rows.SelectedItems().Clear();

        for (auto const trackId : _trackList.selection())
        {
          auto const optIndex = _trackList.displayIndexOfTrack(trackId);

          if (optIndex && *optIndex < items.Size())
          {
            _rows.SelectedItems().Append(items.GetAt(static_cast<std::uint32_t>(*optIndex)));
          }
        }

        _headers.ItemsSource(_trackList.headers());
        _surface.Width(_trackList.contentWidth());

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

        if (!optReveal || optReveal->serial == _lastRevealSerial ||
            optReveal->displayIndex >= _trackList.items().Size())
        {
          return;
        }

        _lastRevealSerial = optReveal->serial;
        auto const item = _trackList.items().GetAt(static_cast<std::uint32_t>(optReveal->displayIndex));
        _rows.SelectedItem(item);
        _rows.ScrollIntoView(item);
      }

      void onViewportSizeChanged(IInspectable const& /*sender*/, SizeChangedEventArgs const& args)
      {
        if (args.NewSize().Width <= 0.0 || !uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        _trackList.setViewportWidth(args.NewSize().Width, _trailingChromeWidth);
        _surface.Width(_trackList.contentWidth());
      }

      void onSelectionChanged(IInspectable const& /*sender*/, SelectionChangedEventArgs const& /*args*/)
      {
        if (_suppressSelectionPublication || !uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        auto selected = std::vector<TrackId>{};

        for (auto const& item : _rows.SelectedItems())
        {
          if (auto const row = item.try_as<ProjectedTrackRowItem>(); row && !row.IsGroupHeader() && row.TrackId() != 0)
          {
            selected.emplace_back(row.TrackId());
          }
        }

        // The selection belongs to the view, not to this control: everything that
        // presents it reads it back from the runtime.
        if (selected != _trackList.selection())
        {
          _trackList.publishSelection(selected);
        }
      }

      void onDoubleTapped(IInspectable const& /*sender*/, DoubleTappedRoutedEventArgs const& args)
      {
        if (!uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        auto const row = trackRowFromEventSource(args.OriginalSource());

        if (!row || row.IsGroupHeader())
        {
          return;
        }

        auto const playedRes = _trackList.play(TrackId{row.TrackId()}, _playTrack);

        if (!playedRes && _reportStatus)
        {
          _reportStatus(formatResource("winui_playback_failed", playedRes.error().message));
        }
      }

      // The context menu is one ordered surface whose branching mirrors its
      // visible sections; keeping the assembly together preserves that order.
      // NOLINTNEXTLINE(readability-function-cognitive-complexity)
      void onRightTapped(IInspectable const& /*sender*/, RightTappedRoutedEventArgs const& args)
      {
        if (!uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        auto const row = trackRowFromEventSource(args.OriginalSource());

        if (!row || row.IsGroupHeader())
        {
          return;
        }

        bool selected = false;

        for (auto const& item : _rows.SelectedItems())
        {
          if (auto const selectedRow = item.try_as<ProjectedTrackRowItem>();
              selectedRow && selectedRow.TrackId() == row.TrackId())
          {
            selected = true;
            break;
          }
        }

        if (!selected)
        {
          _rows.SelectedItem(row);
        }

        if (_contextFlyout)
        {
          _contextClickRevokers.clear();
          _contextFlyout.Hide();
        }

        _contextFlyout = MenuFlyout{};
        auto const appendItem = [this](auto const& items,
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
                         [this, callback = std::move(callback)](
                           IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                         {
                           if (uimodel::isGenerationActive(_gatePtr))
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
                           [this](IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
                           {
                             if (uimodel::isGenerationActive(_gatePtr))
                             {
                               std::ignore = _actions.invoke("track.presentProperties", ActionContext{});
                             }
                           }));
        _contextFlyout.Items().Append(properties);

        if (_membershipTargets && _editMembership)
        {
          auto const targets = _membershipTargets();
          auto addSubmenu = MenuFlyoutSubItem{};
          addSubmenu.Text(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListAddToPlaylist)));
          std::size_t addCount = 0;
          auto const activeListId = _trackList.activeListId();

          for (auto const& target : targets)
          {
            if (target.listId == activeListId)
            {
              continue;
            }

            appendItem(addSubmenu.Items(),
                       std::format("{} ({})", target.name, tagExpression(target.tag)),
                       [edit = _editMembership, listId = target.listId] { edit(listId, true); });
            ++addCount;
          }

          if (addCount == 0)
          {
            appendItem(addSubmenu.Items(), _textCatalog.text(i18n::MessageId::WinUiListNoEditablePlaylists), {}, false);
          }

          _contextFlyout.Items().Append(addSubmenu);

          if (auto const current = std::ranges::find(targets, activeListId, &uimodel::WritableTagListTarget::listId);
              current != targets.end())
          {
            appendItem(_contextFlyout.Items(),
                       _textCatalog.format(i18n::MessageId::WinUiListRemoveFromCurrent,
                                           {i18n::MessageArgument{"name", current->name},
                                            i18n::MessageArgument{"tag", tagExpression(current->tag)}}),
                       [edit = _editMembership, listId = current->listId] { edit(listId, false); });
          }
        }

        if (_orderCapabilities && _applyOrder)
        {
          _contextFlyout.Items().Append(MenuFlyoutSeparator{});
          auto ordering = MenuFlyoutSubItem{};
          ordering.Text(winrt::to_hstring(_textCatalog.text(i18n::MessageId::WinUiListManualOrder)));

          if (auto const capabilities = _orderCapabilities(); !capabilities.canAuthorOrder)
          {
            appendItem(ordering.Items(), capabilities.disabledReason, {}, false);
          }
          else
          {
            auto const action = [this](std::string_view const id)
            { return [this, id = std::string{id}] { std::ignore = _actions.invoke(id, ActionContext{}); }; };
            appendItem(ordering.Items(),
                       _textCatalog.text(i18n::MessageId::WinUiListMoveUp),
                       action("track.orderMoveUp"),
                       capabilities.canRelativeMove,
                       "Alt+Up");
            appendItem(ordering.Items(),
                       _textCatalog.text(i18n::MessageId::WinUiListMoveDown),
                       action("track.orderMoveDown"),
                       capabilities.canRelativeMove,
                       "Alt+Down");
            appendItem(ordering.Items(),
                       _textCatalog.text(i18n::MessageId::WinUiListMoveToTop),
                       action("track.orderMoveToTop"),
                       capabilities.canAbsoluteMove,
                       "Alt+Home");
            appendItem(ordering.Items(),
                       _textCatalog.text(i18n::MessageId::WinUiListMoveToBottom),
                       action("track.orderMoveToBottom"),
                       capabilities.canAbsoluteMove,
                       "Alt+End");
            appendItem(
              ordering.Items(),
              _textCatalog.text(i18n::MessageId::WinUiListResetOrder),
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
      ListView _rows{};
      TrackListController& _trackList;
      std::function<Result<>(rt::ViewId, TrackId)> _playTrack;
      ActionRegistry const& _actions;
      std::function<std::vector<uimodel::WritableTagListTarget>()> _membershipTargets;
      std::function<void(ListId, bool)> _editMembership;
      std::function<uimodel::ListOrderCapabilityState()> _orderCapabilities;
      std::function<void(ListOrderCommand)> _applyOrder;
      uimodel::PresentationTextCatalog _textCatalog;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::function<void(std::string)> _reportStatus;
      double _trailingChromeWidth = kFallbackScrollBarSize;
      std::uint64_t _lastRevealSerial = 0;
      bool _suppressSelectionPublication = false;
      async::Subscription _trackListChangedSub;
      ListView::SelectionChanged_revoker _selectionChangedRevoker{};
      ListView::DoubleTapped_revoker _doubleTappedRevoker{};
      ListView::RightTapped_revoker _rightTappedRevoker{};
      FrameworkElement::SizeChanged_revoker _sizeChangedRevoker{};
      MenuFlyout _contextFlyout{nullptr};
      std::vector<MenuFlyoutItem::Click_revoker> _contextClickRevokers;
    };
  } // namespace

  Result<std::unique_ptr<LayoutComponent>> makeTrackTable(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
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

    return std::make_unique<TrackTableComponent>(ctx,
                                                 headerTemplate,
                                                 rowTemplate,
                                                 lookupResource(ctx.resources, kRowContainerStyleKey).try_as<Style>(),
                                                 winrt::unbox_value_or<double>(scrollBarSize, kFallbackScrollBarSize));
  }
} // namespace ao::winui::layout
