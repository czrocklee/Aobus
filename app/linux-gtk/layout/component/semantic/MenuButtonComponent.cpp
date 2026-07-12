// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/menubutton.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief app.menuButton
     */
    class MenuButtonComponent final : public LayoutComponent
    {
    public:
      MenuButtonComponent(LayoutBuildContext& ctx, LayoutNode const& node)
      {
        if (auto const icon = node.propertyOr<std::string>("icon", ""); !icon.empty())
        {
          _button.set_icon_name(icon);
        }

        auto const style = node.propertyOr<std::string>("style", "flat");

        if (style == "flat")
        {
          _button.set_has_frame(false);
        }

        if (ctx.dependencies.menuModelPtr)
        {
          _button.set_menu_model(ctx.dependencies.menuModelPtr);
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::MenuButton _button;
    };

    std::unique_ptr<LayoutComponent> createMenuButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<MenuButtonComponent>(ctx, node);
    }
  } // namespace

  void registerMenuButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "app.menuButton",
       .displayName = "Menu Button",
       .category = LayoutComponentCategory::Application,
       .props = {{.name = "icon", .kind = LayoutPropertyKind::String, .label = "Icon (Symbolic)"},
                 {.name = "style",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Style",
                  .defaultValue = LayoutValue{"flat"},
                  .enumValues = {"flat", "raised"}}},
       .layoutProps = {},
       .minChildren = 0,
       .optMaxChildren = 0},
      createMenuButton);
  }
} // namespace ao::gtk::layout
