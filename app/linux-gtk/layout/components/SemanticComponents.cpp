// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponents.h"

#include "inspector/CoverArtCache.h"
#include "inspector/CoverArtWidget.h"
#include "inspector/TrackInspectorPanel.h"
#include "list/ListSidebarController.h"
#include "tag/TagEditController.h"
#include "track/TrackPageManager.h"
#include "track/TrackRowCache.h"
#include "track/TrackViewPage.h"
#include <runtime/AppSession.h>
#include <runtime/ListSourceStore.h>
#include <runtime/ViewService.h>
#include <runtime/WorkspaceService.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>

#include "Containers.h"

namespace ao::gtk::layout
{
  namespace
  {
    ListId allTracksListId()
    {
      return ListId{std::numeric_limits<std::uint32_t>::max()};
    }

    /**
     * @brief library.listTree
     */
    class ListTreeComponent final : public ILayoutComponent
    {
    public:
      ListTreeComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.track.trackRowCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: trackRowCache missing");
          return;
        }

        if (ctx.list.sidebarController == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: listSidebarController missing");
          return;
        }

        _controller = ctx.list.sidebarController;

        // Initial rebuild
        auto const txn = ctx.session.musicLibrary().readTransaction();
        _controller->rebuildTree(*ctx.track.trackRowCache, txn);
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : _controller->widget();
      }

    private:
      ListSidebarController* _controller = nullptr;
      Gtk::Label* _error = nullptr;
    };

    /**
     * @brief tracks.table
     */
    class TracksTableComponent final : public ILayoutComponent
    {
    public:
      TracksTableComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.track.pageManager == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageManager missing"));
          return;
        }

        _container.append(ctx.track.pageManager->stack());
        _container.set_hexpand(true);
        _container.set_vexpand(true);
      }

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::VERTICAL};
    };

    /**
     * @brief library.openLibraryButton
     */
    class OpenLibraryButton final : public ILayoutComponent
    {
    public:
      OpenLibraryButton(LayoutDependencies& /*ctx*/, LayoutNode const& /*node*/)
      {
        _button.set_label("Open Library...");
        _button.set_icon_name("folder-open-symbolic");
        _button.signal_clicked().connect(
          []
          {
            // This usually triggers a dialog in MainWindow
          });
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
    };

    /**
     * @brief inspector.coverArt
     */
    class CoverArtComponent final : public ILayoutComponent
    {
    public:
      CoverArtComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.inspector.coverArtCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: coverArtCache missing");
          return;
        }

        _widget = std::make_unique<CoverArtWidget>(ctx.session.musicLibrary(), *ctx.inspector.coverArtCache);
        _widget->bindToDetailProjection(ctx.session.views().detailProjection(
          rt::FocusedViewTarget{}, ctx.session.workspace(), ctx.session.mutation()));
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(*_widget);
      }

    private:
      std::unique_ptr<CoverArtWidget> _widget;
      Gtk::Label* _error = nullptr;
    };

    /**
     * @brief inspector.sidebar
     */
    class InspectorSidebarComponent final : public ILayoutComponent
    {
    public:
      InspectorSidebarComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.inspector.coverArtCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: coverArtCache missing");
          return;
        }

        _widget = std::make_unique<TrackInspectorPanel>(
          ctx.session.musicLibrary(), ctx.session.mutation(), ctx.session.sources(), *ctx.inspector.coverArtCache);
        _widget->bindToDetailProjection(ctx.session.views().detailProjection(
          rt::FocusedViewTarget{}, ctx.session.workspace(), ctx.session.mutation()));

        if (ctx.tag.editController != nullptr)
        {
          _widget->signalTagEditRequested().connect(
            [ctx](std::vector<TrackId> const& ids, Gtk::Widget* relativeTo)
            {
              if (relativeTo != nullptr)
              {
                auto const listId =
                  (ctx.track.pageManager != nullptr) ? ctx.track.pageManager->activeListId() : allTracksListId();

                auto const selection = TrackSelectionContext{.listId = listId, .selectedIds = ids};
                ctx.tag.editController->showTagEditor(selection, *relativeTo);
              }
            });
        }
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(*_widget);
      }

    private:
      std::unique_ptr<TrackInspectorPanel> _widget;
      Gtk::Label* _error = nullptr;
    };

    /**
     * @brief app.menuBar
     */
    class MenuBarComponent final : public ILayoutComponent
    {
    public:
      MenuBarComponent(LayoutDependencies& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.shell.menuModel != nullptr)
        {
          _menuBar.set_menu_model(ctx.shell.menuModel);
        }
      }

      Gtk::Widget& widget() override { return _menuBar; }

    private:
      Gtk::PopoverMenuBar _menuBar;
    };

    /**
     * @brief app.workspaceWithInspector
     *
     * Transitional composite that replicates the current stack + inspector handle + revealer.
     */
    class WorkspaceWithInspectorComponent final : public ILayoutComponent
    {
    public:
      WorkspaceWithInspectorComponent(LayoutDependencies& ctx, LayoutNode const& node)
      {
        if (ctx.track.pageManager == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageManager missing"));
          return;
        }

        _container.set_orientation(Gtk::Orientation::HORIZONTAL);
        _container.set_hexpand(true);
        _container.set_vexpand(true);

        auto& stack = ctx.track.pageManager->stack();
        stack.set_hexpand(true);
        stack.set_vexpand(true);
        _container.append(stack);

        // Handle
        _handle.set_icon_name("pan-start-symbolic");
        _handle.add_css_class("inspector-handle");
        _handle.set_valign(Gtk::Align::CENTER);
        _handle.set_focus_on_click(false);
        _container.append(_handle);

        // Inspector side panel.
        // Matching original MainWindow setupLayout order and properties.
        _inspector = std::make_unique<InspectorSidebarComponent>(ctx, node);
        _inspector->widget().set_vexpand(true);
        _revealer.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
        _revealer.set_child(_inspector->widget());
        _revealer.set_reveal_child(false);
        _revealer.set_hexpand(false);
        _revealer.set_vexpand(true);
        _container.append(_revealer);

        _handle.signal_toggled().connect(
          [this]
          {
            bool const active = _handle.get_active();
            _revealer.set_reveal_child(active);
            _handle.set_icon_name(active ? "pan-end-symbolic" : "pan-start-symbolic");
          });
      }

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container;
      Gtk::ToggleButton _handle;
      Gtk::Revealer _revealer;
      std::unique_ptr<ILayoutComponent> _inspector;
    };
  } // namespace

  void registerSemanticComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "library.listTree",
                                .displayName = "Library Tree",
                                .category = "Library",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<ListTreeComponent>(ctx, node); });

    registry.registerComponent({.type = "tracks.table",
                                .displayName = "Tracks Table",
                                .category = "Tracks",
                                .container = false,
                                .props = {{.name = "view",
                                           .kind = PropertyKind::String,
                                           .label = "View Source",
                                           .defaultValue = LayoutValue{"workspace.focused"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TracksTableComponent>(ctx, node); });

    registry.registerComponent({.type = "library.openLibraryButton",
                                .displayName = "Open Library Button",
                                .category = "Library",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<OpenLibraryButton>(ctx, node); });

    registry.registerComponent({.type = "inspector.coverArt",
                                .displayName = "Cover Art",
                                .category = "Inspector",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<CoverArtComponent>(ctx, node); });

    registry.registerComponent({.type = "inspector.sidebar",
                                .displayName = "Inspector Sidebar",
                                .category = "Inspector",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<InspectorSidebarComponent>(ctx, node); });

    registry.registerComponent({.type = "app.menuBar",
                                .displayName = "Menu Bar",
                                .category = "Application",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<MenuBarComponent>(ctx, node); });

    registry.registerComponent({.type = "app.workspaceWithInspector",
                                .displayName = "Workspace w/ Inspector",
                                .category = "Application",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .maxChildren = 0},
                               [](LayoutDependencies& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<WorkspaceWithInspectorComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
