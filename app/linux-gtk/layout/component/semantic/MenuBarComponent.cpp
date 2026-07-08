// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/popovermenubar.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief app.menuBar
     */
    class MenuBarComponent final : public LayoutComponent
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

    std::unique_ptr<LayoutComponent> createMenuBar(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<MenuBarComponent>(ctx, node);
    }
  } // namespace

  void registerMenuBarComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "app.menuBar",
                                .displayName = "Menu Bar",
                                .category = LayoutComponentCategory::Application,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createMenuBar);
  }
} // namespace ao::gtk::layout
