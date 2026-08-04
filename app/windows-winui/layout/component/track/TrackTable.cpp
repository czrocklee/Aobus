// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackTable.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
    using winrt::Microsoft::UI::Xaml::Controls::ListView;
    using winrt::Microsoft::UI::Xaml::Controls::ListViewSelectionMode;
    using winrt::Microsoft::UI::Xaml::Controls::RowDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollMode;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollViewer;
    using winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs;
    using winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs;
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

      FrameworkElement element() const override { return _viewport; }

    private:
      void refreshTrackList()
      {
        _rows.ItemsSource(_trackList.items());
        _headers.ItemsSource(_trackList.headers());
        _surface.Width(_trackList.contentWidth());
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
        if (!uimodel::isGenerationActive(_gatePtr))
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
        _trackList.publishSelection(selected);
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

        auto const played = _trackList.play(TrackId{row.TrackId()}, _playTrack);

        if (!played && _reportStatus)
        {
          _reportStatus(formatResource("PlaybackFailedFormat", played.error().message));
        }
      }

      ScrollViewer _viewport{};
      Grid _surface{};
      ItemsControl _headers{};
      ListView _rows{};
      TrackListController& _trackList;
      std::function<Result<>(rt::ViewId, TrackId)> _playTrack;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::function<void(std::string)> _reportStatus;
      double _trailingChromeWidth = kFallbackScrollBarSize;
      async::Subscription _trackListChangedSub;
      ListView::SelectionChanged_revoker _selectionChangedRevoker{};
      ListView::DoubleTapped_revoker _doubleTappedRevoker{};
      FrameworkElement::SizeChanged_revoker _sizeChangedRevoker{};
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
