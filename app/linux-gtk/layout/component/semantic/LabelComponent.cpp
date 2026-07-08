// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief A simple text label component.
     */
    class LabelComponent final : public LayoutComponent
    {
    public:
      LabelComponent(LayoutContext& /*ctx*/, LayoutNode const& node)
      {
        if (auto const it = node.props.find("label"); it != node.props.end())
        {
          _label.set_text(it->second.asString());
        }
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };

    std::unique_ptr<LayoutComponent> createLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<LabelComponent>(ctx, node);
    }
  } // namespace

  void registerLabelComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "label",
                                .displayName = "Text Label",
                                .category = LayoutComponentCategory::Generic,
                                .props = {{.name = "label", .kind = LayoutPropertyKind::String, .label = "Text"}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createLabel);
  }
} // namespace ao::gtk::layout
