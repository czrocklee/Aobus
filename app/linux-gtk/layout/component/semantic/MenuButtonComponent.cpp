// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gtkmm/menubutton.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief app.menuButton
     */
    class MenuButtonComponent final : public ILayoutComponent
    {
    public:
      MenuButtonComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (auto const icon = node.getProp<std::string>("icon", ""); !icon.empty())
        {
          _button.set_icon_name(icon);
        }

        auto const style = node.getProp<std::string>("style", "flat");

        if (style == "flat")
        {
          _button.set_has_frame(false);
        }

        if (ctx.shell.menuModelPtr)
        {
          _button.set_menu_model(ctx.shell.menuModelPtr);
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::MenuButton _button;
    };

    std::unique_ptr<ILayoutComponent> createMenuButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<MenuButtonComponent>(ctx, node);
    }
  } // namespace

  void registerMenuButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "app.menuButton",
                                .displayName = "Menu Button",
                                .category = ComponentCategory::Application,
                                .props = {{.name = "icon", .kind = PropertyKind::String, .label = "Icon (Symbolic)"},
                                          {.name = "style",
                                           .kind = PropertyKind::Enum,
                                           .label = "Style",
                                           .defaultValue = LayoutValue{"flat"},
                                           .enumValues = {"flat", "raised"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createMenuButton);
  }
} // namespace ao::gtk::layout
