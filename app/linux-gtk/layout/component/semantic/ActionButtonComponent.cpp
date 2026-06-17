// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ComponentActionPolicy.h>
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    /**
     * @brief app.actionButton
     */
    class ActionButtonComponent final : public ILayoutComponent
    {
    public:
      ActionButtonComponent(LayoutContext& /*ctx*/, LayoutNode const& node)
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
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
    };

    std::unique_ptr<ILayoutComponent> createActionButton(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<ActionButtonComponent>(ctx, node);
    }
  } // namespace

  void registerActionButtonComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "app.actionButton",
                                .displayName = "Action Button",
                                .category = ComponentCategory::Generic,
                                .props = {{.name = "label", .kind = PropertyKind::String, .label = "Text"},
                                          {.name = "icon", .kind = PropertyKind::String, .label = "Icon (Symbolic)"},
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
                                .optMaxChildren = 0,
                                .actionPolicy = uimodel::layout::kExternalPrimaryActions},
                               createActionButton);
  }
} // namespace ao::gtk::layout
