// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
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
     * @brief app.actionButton
     */
    class ActionButtonComponent final : public LayoutComponent
    {
    public:
      ActionButtonComponent(LayoutBuildContext& /*ctx*/, LayoutNode const& node)
      {
        if (auto const label = node.propertyOr<std::string>("label", ""); !label.empty())
        {
          _button.set_label(label);
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
    registry.registerComponent(
      {.type = "app.actionButton",
       .displayName = "Action Button",
       .category = LayoutComponentCategory::Generic,
       .props = {{.name = "label", .kind = LayoutPropertyKind::String, .label = "Text"},
                 {.name = "icon", .kind = LayoutPropertyKind::String, .label = "Icon (Symbolic)"},
                 {.name = "size",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Size",
                  .defaultValue = LayoutValue{"normal"},
                  .enumValues = {"small", "normal", "large"}},
                 {.name = "style",
                  .kind = LayoutPropertyKind::Enum,
                  .label = "Style",
                  .defaultValue = LayoutValue{"flat"},
                  .enumValues = {"flat", "raised", "circular", "suggested", "destructive"}}},
       .minChildren = 0,
       .optMaxChildren = 0,
       .actionPolicy = uimodel::kExternalPrimaryActions},
      createActionButton);
  }
} // namespace ao::gtk::layout
