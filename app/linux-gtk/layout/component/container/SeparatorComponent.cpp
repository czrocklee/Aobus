// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/separator.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief A visual separator component (Gtk::Separator).
     */
    class SeparatorComponent final : public ILayoutComponent
    {
    public:
      SeparatorComponent(LayoutContext& /*ctx*/, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::HORIZONTAL;

        if (node.getProp<std::string>("orientation", "") == "vertical")
        {
          orientation = Gtk::Orientation::VERTICAL;
        }

        _separator.set_orientation(orientation);
      }

      Gtk::Widget& widget() override { return _separator; }

    private:
      Gtk::Separator _separator;
    };

    std::unique_ptr<ILayoutComponent> createSeparator(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SeparatorComponent>(ctx, node);
    }
  } // namespace

  void registerSeparatorComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "separator",
                                .displayName = "Separator",
                                .category = LayoutComponentCategory::Container,
                                .props = {{.name = "orientation",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"horizontal"},
                                           .enumValues = {"horizontal", "vertical"}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSeparator);
  }
} // namespace ao::gtk::layout
