// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gtkmm/popovermenubar.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
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

    std::unique_ptr<ILayoutComponent> createMenuBar(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<MenuBarComponent>(ctx, node);
    }
  } // namespace

  void registerMenuBarComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "app.menuBar",
                                .displayName = "Menu Bar",
                                .category = ComponentCategory::Application,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createMenuBar);
  }
} // namespace ao::gtk::layout
