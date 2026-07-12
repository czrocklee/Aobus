// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>

#include <memory>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief app.workspaceWithDetailPane
     *
     * Transitional composite that replicates the current stack + detail handle + revealer.
     */
    class WorkspaceWithDetailPaneComponent final : public LayoutComponent
    {
    public:
      WorkspaceWithDetailPaneComponent(LayoutBuildContext& ctx, LayoutNode const& node)
      {
        if (ctx.dependencies.trackPageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        _container.set_orientation(Gtk::Orientation::HORIZONTAL);
        _container.set_hexpand(true);
        _container.set_vexpand(true);

        auto& stack = ctx.dependencies.trackPageHost->stack();
        stack.set_hexpand(true);
        stack.set_vexpand(true);
        _container.append(stack);

        // Handle
        _handle.set_icon_name("pan-start-symbolic");
        _handle.add_css_class("ao-detail-handle");
        _handle.set_valign(Gtk::Align::CENTER);
        _handle.set_focus_on_click(false);
        _container.append(_handle);

        if (!node.children.empty())
        {
          _detailPtr = ctx.registry.create(ctx, node.children.front());
        }

        if (_detailPtr)
        {
          _detailPtr->widget().set_vexpand(true);
          _revealer.set_transition_type(Gtk::RevealerTransitionType::SLIDE_LEFT);
          _revealer.set_child(_detailPtr->widget());
          _revealer.set_reveal_child(false);
          _revealer.set_hexpand(false);
          _revealer.set_vexpand(true);
          _container.append(_revealer);
        }

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
      std::unique_ptr<LayoutComponent> _detailPtr;
    };

    std::unique_ptr<LayoutComponent> createWorkspaceWithDetailPane(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<WorkspaceWithDetailPaneComponent>(ctx, node);
    }
  } // namespace

  void registerWorkspaceWithDetailPaneComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "workspace.withDetailPane",
                                .displayName = "Workspace with Detail",
                                .category = LayoutComponentCategory::Layout,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 1},
                               createWorkspaceWithDetailPane);
  }
} // namespace ao::gtk::layout
