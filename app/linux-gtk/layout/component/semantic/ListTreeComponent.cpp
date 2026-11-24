// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "list/ListNavigationController.h"
#include <ao/Contract.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief library.listTree
     */
    class ListTreeComponent final : public LayoutComponent
    {
    public:
      ListTreeComponent(LayoutBuildContext& ctx, ListNavigationController* listNavigationController)
      {
        if (listNavigationController == nullptr)
        {
          _container.append(*Gtk::make_managed<Gtk::Label>("Error: listNavigationController missing"));
          return;
        }

        // Give the sole child the full allocation without changing its expansion flags.
        _container.set_homogeneous(true);
        // The window initializes and refreshes this model independently of the layout.
        _panel = &listNavigationController->widget();

        if (ctx.sharedWidgetHandoff != nullptr)
        {
          ctx.sharedWidgetHandoff->transfer(*_panel, _container);
        }
        else
        {
          AO_EXPECTS(_panel->get_parent() == nullptr,
                     "library.listTree cannot attach an already-parented navigation panel outside a build");
          _container.append(*_panel);
        }
      }

      ~ListTreeComponent() override
      {
        if (_panel != nullptr && _panel->get_parent() == &_container)
        {
          _container.remove(*_panel);
        }
      }

      ListTreeComponent(ListTreeComponent const&) = delete;
      ListTreeComponent& operator=(ListTreeComponent const&) = delete;
      ListTreeComponent(ListTreeComponent&&) = delete;
      ListTreeComponent& operator=(ListTreeComponent&&) = delete;

      Gtk::Widget& widget() override { return _container; }

    private:
      Gtk::Box _container{Gtk::Orientation::VERTICAL};
      Gtk::Widget* _panel = nullptr;
    };
  } // namespace

  void registerListTreeComponent(ComponentRegistry& registry, ListNavigationController* listNavigationController)
  {
    registry.registerComponent({.id = "library.listTree",
                                .displayName = "Library Tree",
                                .category = ComponentCategory::Library,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               [listNavigationController](LayoutBuildContext& ctx, LayoutNode const& /*node*/)
                               { return std::make_unique<ListTreeComponent>(ctx, listNavigationController); });
  }
} // namespace ao::gtk::layout
