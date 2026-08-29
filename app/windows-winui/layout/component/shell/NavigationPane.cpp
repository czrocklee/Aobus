// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/shell/PaneSplitter.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "platform/ScopedBooleanFlag.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/list/ListActions.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/layout/ShellState.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

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
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationView;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationViewBackButtonVisible;
    using winrt::Microsoft::UI::Xaml::Controls::NavigationViewBackRequestedEventArgs;
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

    template<typename NativeItem>
    void expandActiveAncestors(std::map<ListId, NativeItem> const& items,
                               std::map<ListId, ListId> const& parentIds,
                               ListId currentId)
    {
      auto remaining = parentIds.size();

      while (remaining-- > 0)
      {
        auto const current = parentIds.find(currentId);

        if (current == parentIds.end() || current->second == kInvalidListId || current->second == currentId)
        {
          return;
        }

        currentId = current->second;

        if (auto const parent = items.find(currentId); parent != items.end())
        {
          parent->second.IsExpanded(true);
        }
      }
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
      NavigationBinding(TrackListController& trackList,
                        rt::WorkspaceService& workspace,
                        std::function<uimodel::ListTreeProjection()> listTreeProjection,
                        std::function<async::Subscription(compat::MoveOnlyFunction<void()>)> subscribeListTreeChanged,
                        std::function<std::optional<rt::TrackPresentationSpec>(ListId)> preferredPresentation,
                        std::function<void(ListId, std::string)> createList,
                        std::function<void(ListId)> editList,
                        std::function<void(ListId, bool)> deleteList,
                        i18n::MessageCatalog textCatalog,
                        std::weak_ptr<uimodel::ShellGenerationGate> gatePtr,
                        std::function<void(std::string)> reportStatus)
        : _trackList{trackList}
        , _workspace{workspace}
        , _listTreeProjection{std::move(listTreeProjection)}
        , _subscribeListTreeChanged{std::move(subscribeListTreeChanged)}
        , _preferredPresentation{std::move(preferredPresentation)}
        , _createList{std::move(createList)}
        , _editList{std::move(editList)}
        , _deleteList{std::move(deleteList)}
        , _textCatalog{std::move(textCatalog)}
        , _gatePtr{std::move(gatePtr)}
        , _reportStatus{std::move(reportStatus)}
      {
      }

      uimodel::ListTreeProjection projection() const { return _listTreeProjection(); }

      ListId activeListId() const { return _trackList.activeListId(); }
      bool canGoBack() const { return _workspace.canGoBack(); }

      async::Subscription subscribeListTreeChanged(compat::MoveOnlyFunction<void()> callback) const
      {
        return _subscribeListTreeChanged ? _subscribeListTreeChanged(std::move(callback)) : async::Subscription{};
      }

      MenuFlyout contextFlyout(uimodel::ListTreeProjectionRow const& row) const
      {
        auto flyout = MenuFlyout{};
        auto const state = uimodel::describeListActions(row.id, !row.childIds.empty());
        auto const append = [&flyout, gatePtr = _gatePtr](std::string_view const label, std::function<void()> callback)
        {
          if (!callback)
          {
            return;
          }

          auto item = MenuFlyoutItem{};
          item.Text(winrt::to_hstring(label));
          item.Click(
            [gatePtr, callback = std::move(callback)](
              winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
            {
              if (uimodel::isGenerationActive(gatePtr))
              {
                callback();
              }
            });
          flyout.Items().Append(item);
        };

        if (state.canCreate && _createList)
        {
          append(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiListNew),
                 [create = _createList, parentId = uimodel::parentForNewSmartList(row.id)] { create(parentId, {}); });
        }

        if (state.canEdit && _editList)
        {
          append(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiListEdit),
                 [edit = _editList, listId = row.id] { edit(listId); });
        }

        if ((state.canEdit && _editList) &&
            ((state.canDelete && _deleteList) || (state.canDeleteSubtree && _deleteList)))
        {
          flyout.Items().Append(MenuFlyoutSeparator{});
        }

        if (state.canDelete && _deleteList)
        {
          append(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiListDelete),
                 [remove = _deleteList, listId = row.id] { remove(listId, false); });
        }
        else if (state.canDeleteSubtree && _deleteList)
        {
          append(i18n::requiredText(_textCatalog, i18n::MessageId::WinUiListDeleteSubtree),
                 [remove = _deleteList, listId = row.id] { remove(listId, true); });
        }

        return flyout;
      }

      void goBack()
      {
        if (auto const backRes = _workspace.goBack(); !backRes)
        {
          report(formatResource("winui_navigation_failed", backRes.error().message));
        }
      }

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
      rt::WorkspaceService& _workspace;
      std::function<uimodel::ListTreeProjection()> _listTreeProjection;
      std::function<async::Subscription(compat::MoveOnlyFunction<void()>)> _subscribeListTreeChanged;
      std::function<std::optional<rt::TrackPresentationSpec>(ListId)> _preferredPresentation;
      std::function<void(ListId, std::string)> _createList;
      std::function<void(ListId)> _editList;
      std::function<void(ListId, bool)> _deleteList;
      i18n::MessageCatalog _textCatalog;
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
      NavigationViewPaneComponent(LayoutBuildContext& ctx,
                                  NavigationBinding binding,
                                  rt::WorkspaceService& workspace,
                                  PaneSettingsAccess paneSettings,
                                  async::Signal<ShellState>& shellStateChanged)
        : _binding{std::move(binding)}
        , _width{std::move(paneSettings), ctx.gatePtr}
        , _splitter{PaneEdge::Trailing,
                    [this](double const change)
                    {
                      _width.resizeBy(change);
                      applyWidth();
                    },
                    [this] { _width.commit(); }}
      {
        _view.IsBackButtonVisible(NavigationViewBackButtonVisible::Visible);
        _backRequestedRevoker =
          _view.BackRequested(winrt::auto_revoke, {this, &NavigationViewPaneComponent::onBackRequested});
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
        _shellStateSub = subscribeUiUpdate(
          shellStateChanged, "NavigationViewPaneComponent", [this](ShellState const state) { applyShellState(state); });
        _workspaceSub = workspace.onChanged(
          [this](rt::WorkspaceChanged const&)
          {
            applyUiUpdate("NavigationViewPaneComponent",
                          [this]
                          {
                            updateHistory();
                            [[maybe_unused]] auto const applying = ScopedBooleanFlag{_applying};
                            selectActive(_itemsById, _binding.activeListId());
                          });
          });
        _listTreeSub = _binding.subscribeListTreeChanged(
          [this] { applyUiUpdate("NavigationViewPaneComponent", [this] { rebuild(); }); });
        updateHistory();
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

      void updateHistory() { _view.IsBackEnabled(_binding.canGoBack()); }

      void rebuild()
      {
        [[maybe_unused]] auto const applying = ScopedBooleanFlag{_applying};
        auto const projection = _binding.projection();
        auto const activeListId = _binding.activeListId();
        auto previousExpansion = std::map<ListId, bool>{};

        for (auto const& [id, item] : _itemsById)
        {
          previousExpansion.emplace(id, item.IsExpanded());
        }

        auto const restore = restoreListTreeState(projection, activeListId, previousExpansion);
        _view.MenuItems().Clear();
        _labelsById.clear();
        _parentIdsById.clear();
        _itemsById.clear();

        for (auto const& [id, row] : projection.rowsById)
        {
          auto item = NavigationViewItem{};
          auto content = navigationContent(row, false);
          content.ContextFlyout(_binding.contextFlyout(row));
          item.Content(content);
          item.Icon(SymbolIcon{navigationSymbol(row)});
          item.Tag(winrt::box_value(id.raw()));
          _itemsById.emplace(id, item);
          _labelsById.emplace(id, winrt::to_string(navigationLabel(row)));
          _parentIdsById.emplace(id, row.parentId);
        }

        for (auto const& [id, row] : projection.rowsById)
        {
          auto const& item = _itemsById.at(id);

          for (auto const childId : row.childIds)
          {
            if (auto const child = _itemsById.find(childId); child != _itemsById.end())
            {
              item.MenuItems().Append(child->second);
            }
          }

          item.IsExpanded(restore.expandedById.at(id));
        }

        for (auto const rootId : projection.rootIds)
        {
          if (auto const root = _itemsById.find(rootId); root != _itemsById.end())
          {
            _view.MenuItems().Append(root->second);
          }
        }

        selectActive(_itemsById, restore.selectedListId);

        if (restore.selectedListId != activeListId)
        {
          if (auto const fallback = _labelsById.find(restore.selectedListId); fallback != _labelsById.end())
          {
            _binding.navigate(restore.selectedListId, fallback->second);
          }
        }
      }

      void selectActive(std::map<ListId, NavigationViewItem> const& items, ListId const activeListId)
      {
        if (auto const active = items.find(activeListId); active != items.end())
        {
          expandActiveAncestors(items, _parentIdsById, activeListId);
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

      void onBackRequested(NavigationView const& /*sender*/, NavigationViewBackRequestedEventArgs const& /*args*/)
      {
        _binding.goBack();
      }

      NavigationView _view{};
      Grid _content{};
      NavigationBinding _binding;
      NavigationWidth _width;
      PaneSplitter _splitter;
      std::map<ListId, std::string> _labelsById;
      std::map<ListId, ListId> _parentIdsById;
      std::map<ListId, NavigationViewItem> _itemsById;
      std::vector<PlacedChild> _children;
      bool _applying = false;
      async::Subscription _shellStateSub;
      async::Subscription _workspaceSub;
      async::Subscription _listTreeSub;
      NavigationView::SelectionChanged_revoker _selectionChangedRevoker{};
      NavigationView::BackRequested_revoker _backRequestedRevoker{};
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
      NavigationTreePaneComponent(LayoutBuildContext& ctx,
                                  winrt::Microsoft::UI::Xaml::DataTemplate const& nodeTemplate,
                                  NavigationBinding binding,
                                  rt::WorkspaceService& workspace,
                                  PaneSettingsAccess paneSettings,
                                  async::Signal<ShellState>& shellStateChanged)
        : _binding{std::move(binding)}
        , _width{std::move(paneSettings), ctx.gatePtr}
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
        _shellStateSub = subscribeUiUpdate(
          shellStateChanged, "NavigationTreePaneComponent", [this](ShellState const state) { applyShellState(state); });
        _workspaceSub = workspace.onChanged(
          [this](rt::WorkspaceChanged const&)
          {
            applyUiUpdate("NavigationTreePaneComponent",
                          [this]
                          {
                            [[maybe_unused]] auto const applying = ScopedBooleanFlag{_applying};
                            selectActive(_nodesById, _binding.activeListId());
                          });
          });
        _listTreeSub = _binding.subscribeListTreeChanged(
          [this] { applyUiUpdate("NavigationTreePaneComponent", [this] { rebuild(); }); });
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
        auto previousExpansion = std::map<ListId, bool>{};

        for (auto const& [id, node] : _nodesById)
        {
          previousExpansion.emplace(id, node.IsExpanded());
        }

        auto const restore = restoreListTreeState(projection, activeListId, previousExpansion);
        _tree.RootNodes().Clear();
        _labelsById.clear();
        _parentIdsById.clear();
        _nodesById.clear();

        for (auto const& [id, row] : projection.rowsById)
        {
          auto node = TreeViewNode{};
          auto content = navigationContent(row, true);
          content.ContextFlyout(_binding.contextFlyout(row));
          node.Content(content);
          _nodesById.emplace(id, node);
          _labelsById.emplace(id, winrt::to_string(navigationLabel(row)));
          _parentIdsById.emplace(id, row.parentId);
        }

        for (auto const& [id, row] : projection.rowsById)
        {
          auto const& node = _nodesById.at(id);

          for (auto const childId : row.childIds)
          {
            if (auto const child = _nodesById.find(childId); child != _nodesById.end())
            {
              node.Children().Append(child->second);
            }
          }

          node.IsExpanded(restore.expandedById.at(id));
        }

        for (auto const rootId : projection.rootIds)
        {
          if (auto const root = _nodesById.find(rootId); root != _nodesById.end())
          {
            _tree.RootNodes().Append(root->second);
          }
        }

        selectActive(_nodesById, restore.selectedListId);

        if (restore.selectedListId != activeListId)
        {
          if (auto const fallback = _labelsById.find(restore.selectedListId); fallback != _labelsById.end())
          {
            _binding.navigate(restore.selectedListId, fallback->second);
          }
        }
      }

      void selectActive(std::map<ListId, TreeViewNode> const& nodes, ListId const activeListId)
      {
        if (auto const active = nodes.find(activeListId); active != nodes.end())
        {
          expandActiveAncestors(nodes, _parentIdsById, activeListId);
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
      std::map<ListId, ListId> _parentIdsById;
      std::map<ListId, TreeViewNode> _nodesById;
      bool _applying = false;
      async::Subscription _shellStateSub;
      async::Subscription _workspaceSub;
      async::Subscription _listTreeSub;
      TreeView::SelectionChanged_revoker _selectionChangedRevoker{};
    };
  } // namespace

  void registerNavigationPaneComponent(
    ComponentRegistry& registry,
    TrackListController& trackList,
    rt::WorkspaceService& workspace,
    std::function<uimodel::ListTreeProjection()> listTreeProjection,
    std::function<async::Subscription(compat::MoveOnlyFunction<void()>)> subscribeListTreeChanged,
    std::function<std::optional<rt::TrackPresentationSpec>(ListId)> preferredPresentation,
    std::function<void(ListId, std::string)> createList,
    std::function<void(ListId)> editList,
    std::function<void(ListId, bool)> deleteList,
    i18n::MessageCatalog textCatalog,
    PaneSettingsAccess paneSettings,
    async::Signal<ShellState>& shellStateChanged,
    std::function<void(std::string)> reportStatus)
  {
    registry.registerComponent(
      "windows.navigationPane",
      [&trackList,
       &workspace,
       listTreeProjection = std::move(listTreeProjection),
       subscribeListTreeChanged = std::move(subscribeListTreeChanged),
       preferredPresentation = std::move(preferredPresentation),
       createList = std::move(createList),
       editList = std::move(editList),
       deleteList = std::move(deleteList),
       textCatalog = std::move(textCatalog),
       paneSettings = std::move(paneSettings),
       &shellStateChanged,
       reportStatus = std::move(reportStatus)](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        auto binding = NavigationBinding{trackList,
                                         workspace,
                                         listTreeProjection,
                                         subscribeListTreeChanged,
                                         preferredPresentation,
                                         createList,
                                         editList,
                                         deleteList,
                                         textCatalog,
                                         ctx.gatePtr,
                                         reportStatus};

        if (node.propertyOr<std::string>("presentation", {}) != kTreePresentation)
        {
          return std::make_unique<NavigationViewPaneComponent>(
            ctx, std::move(binding), workspace, paneSettings, shellStateChanged);
        }

        auto const boxedKey = winrt::box_value(winrt::to_hstring(kTreeNodeTemplateKey));
        auto const nodeTemplate = ctx.resources && ctx.resources.HasKey(boxedKey)
                                    ? ctx.resources.Lookup(boxedKey).try_as<winrt::Microsoft::UI::Xaml::DataTemplate>()
                                    : nullptr;

        if (!nodeTemplate)
        {
          return makeError(Error::Code::NotFound,
                           std::format("Node '{}' needs the window resource '{}', which the frame does not declare",
                                       node.id,
                                       kTreeNodeTemplateKey));
        }

        return std::make_unique<NavigationTreePaneComponent>(
          ctx, nodeTemplate, std::move(binding), workspace, paneSettings, shellStateChanged);
      });
  }
} // namespace ao::winui::layout
