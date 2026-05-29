// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponents.h"

#include "image/ImageWidget.h"
#include "inspector/TrackInspectorPanel.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ActionBinder.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include "list/ListNavigationController.h"
#include "tag/TagEditController.h"
#include "track/TrackPageHost.h"
#include "track/TrackViewPage.h"
#include <ao/Type.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/gesturelongpress.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>
#include <vector>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief library.listTree
     */
    class ListTreeComponent final : public ILayoutComponent
    {
    public:
      ListTreeComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
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
        auto const txn = ctx.runtime.musicLibrary().readTransaction();
        _controller->rebuildTree(*ctx.track.trackRowCache, txn);
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : _controller->widget();
      }

    private:
      ListNavigationController* _controller = nullptr;
      Gtk::Label* _error = nullptr;
    };

    /**
     * @brief tracks.table
     */
    class TracksTableComponent final : public ILayoutComponent
    {
    public:
      TracksTableComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.track.pageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        _container.append(ctx.track.pageHost->stack());
        _container.set_hexpand(true);
        _container.set_vexpand(true);
      }

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::VERTICAL};
    };

    /**
     * @brief A simple text label component.
     */
    class LabelComponent final : public ILayoutComponent
    {
    public:
      LabelComponent(LayoutContext& /*ctx*/, LayoutNode const& node)
      {
        if (auto const it = node.props.find("label"); it != node.props.end())
        {
          _label.set_text(it->second.asString());
        }
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };

    /**
     * @brief app.actionButton
     */
    class ActionButtonComponent final : public ILayoutComponent
    {
    public:
      ActionButtonComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (auto const label = node.getProp<std::string>("label", ""); !label.empty())
        {
          _button.set_label(label);
        }

        if (auto const icon = node.getProp<std::string>("icon", ""); !icon.empty())
        {
          _button.set_icon_name(icon);
        }

        auto const style = node.getProp<std::string>("style", "standard");

        if (style == "flat")
        {
          _button.set_has_frame(false);
        }
        else if (style == "circular")
        {
          _button.add_css_class("circular");
        }
        else if (style == "suggested")
        {
          _button.add_css_class("suggested-action");
        }
        else if (style == "destructive")
        {
          _button.add_css_class("destructive-action");
        }

        auto const size = node.getProp<std::string>("size", "normal");

        if (size == "small")
        {
          _button.add_css_class("playback-button-small");
        }
        else if (size == "large")
        {
          _button.add_css_class("playback-button-large");
        }

        auto const binder = ActionBinder{ctx.actionRegistry, ctx.runtime, ctx.parentWindow};

        auto const primaryActionCb = binder.bind(node, "primaryAction", "none", ActionSlot::PrimaryClick, _button);

        if (primaryActionCb)
        {
          _button.signal_clicked().connect(
            [this, primaryActionCb]
            {
              if (_longPressPtrHandled)
              {
                _longPressPtrHandled = false;
                return;
              }

              primaryActionCb();
            });
        }

        auto const primaryLongPressCb =
          binder.bind(node, "primaryLongPressAction", "none", ActionSlot::PrimaryLongPress, _button);

        if (primaryLongPressCb)
        {
          _longPressPtr = Gtk::GestureLongPress::create();
          _longPressPtr->signal_pressed().connect(
            [this, primaryLongPressCb](double /*x*/, double /*y*/)
            {
              _longPressPtrHandled = true;
              primaryLongPressCb();
            });
          _button.add_controller(_longPressPtr);
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
      Glib::RefPtr<Gtk::GestureLongPress> _longPressPtr;
      bool _longPressPtrHandled = false;
    };

    /**
     * @brief library.openLibraryButton
     */
    class OpenLibraryButton final : public ILayoutComponent
    {
    public:
      OpenLibraryButton(LayoutContext& /*ctx*/, LayoutNode const& /*node*/)
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
     * @brief inspector.image
     */
    class ImageComponent final : public ILayoutComponent
    {
    public:
      ImageComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.inspector.imageCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: imageCache missing");
          return;
        }

        _widgetPtr = std::make_unique<ImageWidget>(ctx.runtime.musicLibrary(), *ctx.inspector.imageCache);
        _widgetPtr->bindToDetailProjection(ctx.runtime.views().detailProjection(
          rt::FocusedViewTarget{}, ctx.runtime.workspace(), ctx.runtime.mutation()));
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(*_widgetPtr);
      }

    private:
      std::unique_ptr<ImageWidget> _widgetPtr;
      Gtk::Label* _error = nullptr;
    };

    /**
     * @brief inspector.sidebar
     */
    class InspectorSidebarComponent final : public ILayoutComponent
    {
    public:
      InspectorSidebarComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.inspector.imageCache == nullptr)
        {
          _error = Gtk::make_managed<Gtk::Label>("Error: imageCache missing");
          return;
        }

        _widgetPtr = std::make_unique<TrackInspectorPanel>(
          ctx.runtime.musicLibrary(), ctx.runtime.mutation(), ctx.runtime.sources(), *ctx.inspector.imageCache);
        _widgetPtr->bindToDetailProjection(ctx.runtime.views().detailProjection(
          rt::FocusedViewTarget{}, ctx.runtime.workspace(), ctx.runtime.mutation()));

        if (ctx.tag.editController != nullptr)
        {
          _widgetPtr->signalTagEditRequested().connect(
            [ctx](std::vector<TrackId> const& ids, Gtk::Widget* relativeTo)
            {
              if (relativeTo != nullptr)
              {
                auto const listId =
                  (ctx.track.pageHost != nullptr) ? ctx.track.pageHost->activeListId() : rt::kAllTracksListId;

                auto const selection = TrackSelectionContext{.listId = listId, .selectedIds = ids};
                ctx.tag.editController->showTagEditor(selection, *relativeTo);
              }
            });
        }
      }

      Gtk::Widget& widget() override
      {
        return (_error != nullptr) ? static_cast<Gtk::Widget&>(*_error) : static_cast<Gtk::Widget&>(*_widgetPtr);
      }

    private:
      std::unique_ptr<TrackInspectorPanel> _widgetPtr;
      Gtk::Label* _error = nullptr;
    };

    /**
     * @brief app.menuBar
     */
    class MenuBarComponent final : public ILayoutComponent
    {
    public:
      MenuBarComponent(LayoutContext& ctx, LayoutNode const& /*node*/)
      {
        if (ctx.shell.menuModelPtr != nullptr)
        {
          _menuBar.set_menu_model(ctx.shell.menuModelPtr);
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
      WorkspaceWithInspectorComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (ctx.track.pageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        _container.set_orientation(Gtk::Orientation::HORIZONTAL);
        _container.set_hexpand(true);
        _container.set_vexpand(true);

        auto& stack = ctx.track.pageHost->stack();
        stack.set_hexpand(true);
        stack.set_vexpand(true);
        _container.append(stack);

        // Handle
        _handle.set_icon_name("pan-start-symbolic");
        _handle.add_css_class("ao-inspector-handle");
        _handle.set_valign(Gtk::Align::CENTER);
        _handle.set_focus_on_click(false);
        _container.append(_handle);

        // Inspector side panel.
        // Matching original MainWindow setupLayout order and properties.
        _inspectorPtr = std::make_unique<InspectorSidebarComponent>(ctx, node);
        _inspectorPtr->widget().set_vexpand(true);
        _revealer.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
        _revealer.set_child(_inspectorPtr->widget());
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
      std::unique_ptr<ILayoutComponent> _inspectorPtr;
    };
  } // namespace

  void registerSemanticComponents(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "label",
                                .displayName = "Text Label",
                                .category = "Generic",
                                .container = false,
                                .props = {{.name = "label", .kind = PropertyKind::String, .label = "Text"}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<LabelComponent>(ctx, node); });

    registry.registerComponent(
      {.type = "app.actionButton",
       .displayName = "Action Button",
       .category = "Generic",
       .container = false,
       .props = {{.name = "label", .kind = PropertyKind::String, .label = "Text"},
                 {.name = "icon", .kind = PropertyKind::String, .label = "Icon (Symbolic)"},
                 {.name = "primaryAction",
                  .kind = PropertyKind::Enum,
                  .label = "Primary Action",
                  .optActionBinding = ActionBindingProperty{.slot = ActionSlot::PrimaryClick}},
                 {.name = "primaryLongPressAction",
                  .kind = PropertyKind::Enum,
                  .label = "Long Press",
                  .optActionBinding = ActionBindingProperty{.slot = ActionSlot::PrimaryLongPress}},
                 {.name = "size",
                  .kind = PropertyKind::Enum,
                  .label = "Size",
                  .defaultValue = LayoutValue{"normal"},
                  .enumValues = {"small", "normal", "large"}},
                 {.name = "style",
                  .kind = PropertyKind::Enum,
                  .label = "Style",
                  .defaultValue = LayoutValue{"flat"},
                  .enumValues = {"flat", "raised", "circular", "suggested", "destructive"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
      { return std::make_unique<ActionButtonComponent>(ctx, node); });

    registry.registerComponent({.type = "library.listTree",
                                .displayName = "Library Tree",
                                .category = "Library",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
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
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<TracksTableComponent>(ctx, node); });

    registry.registerComponent({.type = "library.openLibraryButton",
                                .displayName = "Open Library Button",
                                .category = "Library",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<OpenLibraryButton>(ctx, node); });

    registry.registerComponent({.type = "inspector.image",
                                .displayName = "Cover Art",
                                .category = "Inspector",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<ImageComponent>(ctx, node); });

    registry.registerComponent({.type = "inspector.sidebar",
                                .displayName = "Inspector Sidebar",
                                .category = "Inspector",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<InspectorSidebarComponent>(ctx, node); });

    registry.registerComponent({.type = "app.menuBar",
                                .displayName = "Menu Bar",
                                .category = "Application",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<MenuBarComponent>(ctx, node); });

    registry.registerComponent({.type = "app.workspaceWithInspector",
                                .displayName = "Workspace w/ Inspector",
                                .category = "Application",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [](LayoutContext& ctx, LayoutNode const& node) -> std::unique_ptr<ILayoutComponent>
                               { return std::make_unique<WorkspaceWithInspectorComponent>(ctx, node); });
  }
} // namespace ao::gtk::layout
