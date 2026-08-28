// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief actionButton
     */
    class ActionButtonComponent final : public LayoutComponent
    {
    public:
      ActionButtonComponent(LayoutBuildContext& /*ctx*/, LayoutNode const& node)
      {
        if (auto const text = node.propertyOr<std::string>(kTextProp, ""); !text.empty())
        {
          _button.set_label(text);
        }

        if (auto const icon = node.propertyOr<std::string>("icon", ""); !icon.empty())
        {
          _button.set_icon_name(icon);
        }

        auto const style = node.propertyOr<std::string>("style", "standard");

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

        auto const size = node.propertyOr<std::string>("size", "normal");

        if (size == "small")
        {
          _button.add_css_class("playback-button-small");
        }
        else if (size == "large")
        {
          _button.add_css_class("playback-button-large");
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
    };

    std::unique_ptr<LayoutComponent> createActionButton(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ActionButtonComponent>(ctx, node);
    }
  } // namespace

  void registerActionButtonComponent(ComponentRegistry& registry)
  {
    registry.registerSharedComponent(
      "actionButton",
      {.properties = {{.name = "icon", .kind = PropertyKind::String, .label = "Icon (Symbolic)"},
                      {.name = "size",
                       .kind = PropertyKind::Enum,
                       .label = "Size",
                       .defaultValue = LayoutValue{"normal"},
                       .enumValues = {"small", "normal", "large"}},
                      {.name = "style",
                       .kind = PropertyKind::Enum,
                       .label = "Style",
                       .defaultValue = LayoutValue{"flat"},
                       .enumValues = {"flat", "raised", "circular", "suggested", "destructive"}}}},
      createActionButton);
  }
} // namespace ao::gtk::layout
