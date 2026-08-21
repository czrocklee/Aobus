// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/shell/NavigationPane.h"

#include "layout/component/shell/PaneSplitter.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "platform/ScopedBooleanFlag.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/ShellStatePolicy.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::TextTrimming;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::Canvas;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationView;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationViewBackButtonVisible;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationViewPaneDisplayMode;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs;
    using winrt::Microsoft::UI::Xaml::Controls::Orientation;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::Symbol;
    using winrt::Microsoft::UI::Xaml::Controls::SymbolIcon;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::TreeView;
    using winrt::Microsoft::UI::Xaml::Controls::TreeViewNode;
    using winrt::Microsoft::UI::Xaml::Controls::TreeViewSelectionChangedEventArgs;

    constexpr auto kTreePresentation = std::string_view{"tree"};
    constexpr auto kTreeNodeTemplateKey = std::string_view{"NavigationTreeNodeTemplate"};
    constexpr double kNavigationItemSpacing = 6.0;
    constexpr double kNavigationFilterOpacity = 0.55;
    constexpr double kCompactPaneLength = 48.0;

    /// The boundary stays above the content it overlaps so a drag always reaches it.
    constexpr std::int32_t kSplitterZIndex = 20;

    Symbol navigationSymbol(uimodel::ListTreeProjectionRow const& row) noexcept
    {
      return row.id == rt::kAllTracksListId ? Symbol::MusicInfo : Symbol::Find;
    }

    winrt::hstring navigationLabel(uimodel::ListTreeProjectionRow const& row)
    {
      return winrt::to_hstring(row.name);
    }

    /**
     * @brief The row a navigation entry shows, tagged with the list it selects.
     *
     * The tag travels with the element rather than with the container so both
     * shell modes recover the same list id from a selection, whatever their
     * own item type is.
     */
    FrameworkElement navigationContent(uimodel::ListTreeProjectionRow const& row, bool const includeIcon)
    {
      auto content = StackPanel{};
      content.Orientation(Orientation::Horizontal);
      content.Spacing(kNavigationItemSpacing);
      content.Tag(winrt::box_value(row.id.raw()));

      if (includeIcon)
      {
        content.Children().Append(SymbolIcon{navigationSymbol(row)});
      }

      auto label = TextBlock{};
      label.Text(navigationLabel(row));
      label.TextTrimming(TextTrimming::CharacterEllipsis);
      content.Children().Append(label);

      if (!row.localExpression.empty())
      {
        auto filter = TextBlock{};
        filter.Text(winrt::to_hstring(std::format("[{}]", row.localExpression)));
        filter.Opacity(kNavigationFilterOpacity);
        filter.TextTrimming(TextTrimming::CharacterEllipsis);
        content.Children().Append(filter);
      }

      return content;
    }

    ListId taggedListId(FrameworkElement const& element)
    {
      return element ? ListId{winrt::unbox_value_or<std::uint32_t>(element.Tag(), kInvalidListId.raw())}
                     : kInvalidListId;
    }

    /**
     * @brief The library tree both shell modes show, and where a selection goes.
     *
     * The binding borrows the session and track list for this window rather
     * than retaining the runtime directly.
     */
    class NavigationBinding final
    {
    public:
      explicit NavigationBinding(LayoutBuildContext& ctx)
        : _trackList{ctx.trackList}
        , _listTreeProjection{ctx.library.listTreeProjection}
        , _preferredPresentation{ctx.library.preferredPresentation}
        , _gatePtr{ctx.gatePtr}
        , _reportStatus{ctx.reportStatus}
      {
      }

      uimodel::ListTreeProjection projection() const { return _listTreeProjection(); }

      ListId activeListId() const { return _trackList.activeListId(); }

      /// Move the workspace to @p listId, restoring the presentation that list was last shown with.
      void navigate(ListId const listId, std::string const& label)
      {
        if (listId == kInvalidListId || !uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        if (auto const navigatedRes = _trackList.navigateTo(listId); !navigatedRes)
        {
          report(formatResource("winui_navigation_failed", navigatedRes.error().message));
          return;
        }

        if (auto const optPresentation = _preferredPresentation(listId); optPresentation)
        {
          if (auto const selectedRes = _trackList.selectPresentation(*optPresentation); !selectedRes)
          {
            report(formatResource("winui_presentation_failed", selectedRes.error().message));
            return;
          }
        }

        report(formatResource("winui_list_status", label));
      }

    private:
      void report(std::string status) const
      {
        if (_reportStatus)
        {
          _reportStatus(std::move(status));
        }
      }

      TrackListController& _trackList;
      std::function<uimodel::ListTreeProjection()> _listTreeProjection;
      std::function<std::optional<rt::TrackPresentationSpec>(ListId)> _preferredPresentation;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::function<void(std::string)> _reportStatus;
    };

    /// The persisted navigation width, clamped and applied by whichever pane owns it.
    class NavigationWidth final
    {
    public:
      NavigationWidth(PaneSettingsAccess settings, std::weak_ptr<uimodel::ShellGenerationGate> gatePtr)
        : _settings{std::move(settings)}, _gatePtr{std::move(gatePtr)}
      {
      }

      bool available() const noexcept { return _settings.navigationWidth && _settings.setNavigationWidth; }

      double value() const { return available() ? _settings.navigationWidth() : kDefaultNavigationPaneWidth; }

      void resizeBy(double const change)
      {
        if (!available() || !uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        _settings.setNavigationWidth(
          std::clamp(_settings.navigationWidth() + change, kMinimumNavigationPaneWidth, kMaximumNavigationPaneWidth));
      }

      void commit() const
      {
        if (_settings.commit && uimodel::isGenerationActive(_gatePtr))
        {
          _settings.commit();
        }
      }

    private:
      PaneSettingsAccess _settings;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
    };

    /**
     * @brief The Modern navigation pane, which owns the workspace as its content.
     *
     * `NavigationView` draws its own pane, so the resolved navigation pane
     * mode becomes a display mode rather than a width the shell
     * computes. The drag boundary lives at the leading edge of the content
     * region, which is the trailing edge of the pane.
     */
    class NavigationViewPaneComponent final : public LayoutContainer
    {
    public:
      explicit NavigationViewPaneComponent(LayoutBuildContext& ctx)
        : _binding{ctx}
        , _width{ctx.paneSettings, ctx.gatePtr}
        , _splitter{PaneEdge::Trailing,
                    [this](double const change)
                    {
                      _width.resizeBy(change);
                      applyWidth();
                    },
                    [this] { _width.commit(); }}
      {
        _view.IsBackButtonVisible(NavigationViewBackButtonVisible::Collapsed);
        _view.IsSettingsVisible(false);
        _view.PaneTitle(winrt::to_hstring(resourceString("winui_library_navigation_pane_title")));
        _view.CompactPaneLength(kCompactPaneLength);
        _selectionChangedRevoker =
          _view.SelectionChanged(winrt::auto_revoke, {this, &NavigationViewPaneComponent::onSelectionChanged});

        auto const splitter = _splitter.element();
        splitter.HorizontalAlignment(HorizontalAlignment::Left);
        Canvas::SetZIndex(splitter, kSplitterZIndex);
        _content.Children().Append(splitter);
        _view.Content(_content);
        applyWidth();
        rebuild();
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(ctx.shellStateChanged,
                                           "NavigationViewPaneComponent",
                                           [this](ShellState const state) { applyShellState(state); });
      }

      FrameworkElement element() const override { return _view; }

      void adopt(std::vector<PlacedChild> children) override
      {
        if (!children.empty())
        {
          _content.Children().Append(children.front().componentPtr->element());
        }

        _children = std::move(children);
      }

    private:
      void applyShellState(ShellState const& state)
      {
        using Navigation = NavigationPaneMode;
        applyWidth();

        if (state.navigation == Navigation::Expanded)
        {
          _view.PaneDisplayMode(NavigationViewPaneDisplayMode::Left);
          _view.IsPaneOpen(true);
        }
        else if (state.navigation == Navigation::Compact)
        {
          _view.PaneDisplayMode(NavigationViewPaneDisplayMode::LeftCompact);
          _view.IsPaneOpen(false);
        }
        else
        {
          _view.PaneDisplayMode(NavigationViewPaneDisplayMode::LeftMinimal);
          _view.IsPaneOpen(false);
        }

        // Only an expanded pane has a boundary the user can move.
        _splitter.setVisible(state.navigation == Navigation::Expanded);
      }

      void applyWidth() { _view.OpenPaneLength(_width.value()); }

      void rebuild()
      {
        [[maybe_unused]] auto const applying = ScopedBooleanFlag{_applying};
        auto const projection = _binding.projection();
        auto const activeListId = _binding.activeListId();
        _view.MenuItems().Clear();
        _labelsById.clear();

        auto items = std::map<ListId, NavigationViewItem>{};

        for (auto const& [id, row] : projection.rowsById)
        {
          auto item = NavigationViewItem{};
          item.Content(navigationContent(row, false));
          item.Icon(SymbolIcon{navigationSymbol(row)});
          item.Tag(winrt::box_value(id.raw()));
          items.emplace(id, item);
          _labelsById.emplace(id, winrt::to_string(navigationLabel(row)));
        }

        for (auto const& [id, row] : projection.rowsById)
        {
          auto const& item = items.at(id);

          for (auto const childId : row.childIds)
          {
            if (auto const child = items.find(childId); child != items.end())
            {
              item.MenuItems().Append(child->second);
            }
          }

          item.IsExpanded(!row.childIds.empty());
        }

        for (auto const rootId : projection.rootIds)
        {
          if (auto const root = items.find(rootId); root != items.end())
          {
            _view.MenuItems().Append(root->second);
          }
        }

        selectActive(items, activeListId);
      }

      void selectActive(std::map<ListId, NavigationViewItem> const& items, ListId const activeListId)
      {
        if (auto const active = items.find(activeListId); active != items.end())
        {
          _view.SelectedItem(active->second);
        }
        else if (auto const fallback = items.find(rt::kAllTracksListId); fallback != items.end())
        {
          _view.SelectedItem(fallback->second);
        }
      }

      void onSelectionChanged(NavigationView const& /*sender*/, NavigationViewSelectionChangedEventArgs const& args)
      {
        if (_applying)
        {
          return;
        }

        auto const listId = taggedListId(args.SelectedItemContainer());

        if (auto const label = _labelsById.find(listId); label != _labelsById.end())
        {
          _binding.navigate(listId, label->second);
        }
      }

      NavigationView _view{};
      Grid _content{};
      NavigationBinding _binding;
      NavigationWidth _width;
      PaneSplitter _splitter;
      std::map<ListId, std::string> _labelsById;
      std::vector<PlacedChild> _children;
      bool _applying = false;
      async::Subscription _shellStateSub;
      NavigationView::SelectionChanged_revoker _selectionChangedRevoker{};
    };

    /**
     * @brief The Classic navigation pane, a tree in a column of its own.
     *
     * The tree hosts no document children, so the pane is a plain component: its
     * width is the column it occupies, and a narrow window collapses it
     * entirely rather than compacting it.
     */
    class NavigationTreePaneComponent final : public LayoutComponent
    {
    public:
      NavigationTreePaneComponent(LayoutBuildContext& ctx, winrt::Microsoft::UI::Xaml::DataTemplate const& nodeTemplate)
        : _binding{ctx}
        , _width{ctx.paneSettings, ctx.gatePtr}
        , _splitter{PaneEdge::Trailing,
                    [this](double const change)
                    {
                      _width.resizeBy(change);
                      applyWidth();
                    },
                    [this] { _width.commit(); }}
      {
        // Without a template a TreeView prints the type name of its node
        // content, so the frame's template is a hard requirement rather than an
        // improvement the pane can do without.
        _tree.ItemTemplate(nodeTemplate);
        _selectionChangedRevoker =
          _tree.SelectionChanged(winrt::auto_revoke, {this, &NavigationTreePaneComponent::onSelectionChanged});

        auto const splitter = _splitter.element();
        splitter.HorizontalAlignment(HorizontalAlignment::Right);
        Canvas::SetZIndex(splitter, kSplitterZIndex);
        _root.Children().Append(_tree);
        _root.Children().Append(splitter);
        applyWidth();
        rebuild();
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(ctx.shellStateChanged,
                                           "NavigationTreePaneComponent",
                                           [this](ShellState const state) { applyShellState(state); });
      }

      FrameworkElement element() const override { return _root; }

    private:
      void applyShellState(ShellState const& state)
      {
        auto const visible = state.widthClass != ShellWidthClass::Narrow;
        _root.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        _splitter.setVisible(visible);
        applyWidth();
      }

      void applyWidth() { _root.Width(_width.value()); }

      void rebuild()
      {
        [[maybe_unused]] auto const applying = ScopedBooleanFlag{_applying};
        auto const projection = _binding.projection();
        auto const activeListId = _binding.activeListId();
        _tree.RootNodes().Clear();
        _labelsById.clear();

        auto nodes = std::map<ListId, TreeViewNode>{};

        for (auto const& [id, row] : projection.rowsById)
        {
          auto node = TreeViewNode{};
          node.Content(navigationContent(row, true));
          nodes.emplace(id, node);
          _labelsById.emplace(id, winrt::to_string(navigationLabel(row)));
        }

        for (auto const& [id, row] : projection.rowsById)
        {
          auto const& node = nodes.at(id);

          for (auto const childId : row.childIds)
          {
            if (auto const child = nodes.find(childId); child != nodes.end())
            {
              node.Children().Append(child->second);
            }
          }

          node.IsExpanded(!row.childIds.empty());
        }

        for (auto const rootId : projection.rootIds)
        {
          if (auto const root = nodes.find(rootId); root != nodes.end())
          {
            _tree.RootNodes().Append(root->second);
          }
        }

        selectActive(nodes, activeListId);
      }

      void selectActive(std::map<ListId, TreeViewNode> const& nodes, ListId const activeListId)
      {
        if (auto const active = nodes.find(activeListId); active != nodes.end())
        {
          _tree.SelectedNode(active->second);
        }
        else if (auto const fallback = nodes.find(rt::kAllTracksListId); fallback != nodes.end())
        {
          _tree.SelectedNode(fallback->second);
        }
      }

      void onSelectionChanged(TreeView const& sender, TreeViewSelectionChangedEventArgs const& /*args*/)
      {
        if (_applying)
        {
          return;
        }

        auto const node = sender.SelectedNode();
        auto const listId = taggedListId(node ? node.Content().try_as<FrameworkElement>() : FrameworkElement{nullptr});

        if (auto const label = _labelsById.find(listId); label != _labelsById.end())
        {
          _binding.navigate(listId, label->second);
        }
      }

      Grid _root{};
      TreeView _tree{};
      NavigationBinding _binding;
      NavigationWidth _width;
      PaneSplitter _splitter;
      std::map<ListId, std::string> _labelsById;
      bool _applying = false;
      async::Subscription _shellStateSub;
      TreeView::SelectionChanged_revoker _selectionChangedRevoker{};
    };
  } // namespace

  Result<std::unique_ptr<LayoutComponent>> makeNavigationPane(LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
  {
    if (node.propertyOr<std::string>("presentation", {}) != kTreePresentation)
    {
      return std::make_unique<NavigationViewPaneComponent>(ctx);
    }

    auto const boxedKey = winrt::box_value(winrt::to_hstring(kTreeNodeTemplateKey));
    auto const nodeTemplate = ctx.resources && ctx.resources.HasKey(boxedKey)
                                ? ctx.resources.Lookup(boxedKey).try_as<winrt::Microsoft::UI::Xaml::DataTemplate>()
                                : nullptr;

    if (!nodeTemplate)
    {
      return makeError(
        Error::Code::NotFound,
        std::format(
          "Node '{}' needs the window resource '{}', which the frame does not declare", node.id, kTreeNodeTemplateKey));
    }

    return std::make_unique<NavigationTreePaneComponent>(ctx, nodeTemplate);
  }
} // namespace ao::winui::layout
