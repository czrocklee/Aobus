// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "common/AccessibleLabel.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include <ao/Contract.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>

#include <memory>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief workspace.withDetailPane
     *
     * Transitional composite that replicates the current stack + detail handle + revealer.
     */
    class WorkspaceWithDetailPaneComponent final : public LayoutComponent
    {
    public:
      WorkspaceWithDetailPaneComponent(TrackPageHost* trackPageHost,
                                       i18n::MessageCatalog textCatalog,
                                       LayoutBuildContext& ctx,
                                       LayoutNode const& node)
        : _textCatalog{std::move(textCatalog)}
      {
        if (trackPageHost == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: trackPageHost missing"));
          return;
        }

        _container.set_orientation(Gtk::Orientation::HORIZONTAL);
        _container.set_hexpand(true);
        _container.set_vexpand(true);

        _stack = &trackPageHost->stack();
        _stack->set_hexpand(true);
        _stack->set_vexpand(true);

        if (ctx.sharedWidgetHandoff != nullptr)
        {
          ctx.sharedWidgetHandoff->transfer(*_stack, _container);
        }
        else
        {
          AO_EXPECTS(_stack->get_parent() == nullptr,
                     "workspace.withDetailPane cannot attach an already-parented TrackPageHost stack outside a build");
          _container.append(*_stack);
        }

        // Handle
        updateHandlePresentation(false);
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
            updateHandlePresentation(active);
          });
      }

      ~WorkspaceWithDetailPaneComponent() override
      {
        if (_stack != nullptr && _stack->get_parent() == &_container)
        {
          _container.remove(*_stack);
        }
      }

      WorkspaceWithDetailPaneComponent(WorkspaceWithDetailPaneComponent const&) = delete;
      WorkspaceWithDetailPaneComponent& operator=(WorkspaceWithDetailPaneComponent const&) = delete;
      WorkspaceWithDetailPaneComponent(WorkspaceWithDetailPaneComponent&&) = delete;
      WorkspaceWithDetailPaneComponent& operator=(WorkspaceWithDetailPaneComponent&&) = delete;

      Gtk::Widget& widget() override { return _container; }

    private:
      void updateHandlePresentation(bool const active)
      {
        _handle.set_icon_name(active ? "pan-end-symbolic" : "pan-start-symbolic");
        auto const id = active ? i18n::MessageId::GtkHideDetails : i18n::MessageId::GtkShowDetails;
        setTooltipAndAccessibleLabel(_handle, gtkText(_textCatalog, id));
      }

      i18n::MessageCatalog _textCatalog;
      Gtk::Box _container;
      Gtk::Stack* _stack = nullptr;
      Gtk::ToggleButton _handle;
      Gtk::Revealer _revealer;
      std::unique_ptr<LayoutComponent> _detailPtr;
    };
  } // namespace

  void registerWorkspaceWithDetailPaneComponent(ComponentRegistry& registry,
                                                TrackPageHost* trackPageHost,
                                                i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "workspace.withDetailPane",
       .displayName = "Workspace with Detail",
       .category = ComponentCategory::Layout,
       .minChildren = 0,
       .optMaxChildren = 1},
      [trackPageHost, textCatalog](LayoutBuildContext& ctx, LayoutNode const& node)
      { return std::make_unique<WorkspaceWithDetailPaneComponent>(trackPageHost, textCatalog, ctx, node); });
  }
} // namespace ao::gtk::layout
