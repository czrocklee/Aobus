// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    /**
     * @brief A box container component.
     */
    class BoxComponent final : public ILayoutComponent
    {
    public:
      BoxComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::VERTICAL;

        if (node.getProp<std::string>("orientation", "") == "horizontal")
        {
          orientation = Gtk::Orientation::HORIZONTAL;
        }

        _box.set_orientation(orientation);
        _box.set_spacing(static_cast<std::int32_t>(node.getProp<std::int64_t>("spacing", 0)));
        _box.set_homogeneous(node.getProp<bool>("homogeneous", false));

        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);
          _box.append(childPtr->widget());
          _children.push_back(std::move(childPtr));
        }
      }

      Gtk::Widget& widget() override { return _box; }

    private:
      Gtk::Box _box;
      std::vector<std::unique_ptr<ILayoutComponent>> _children;
    };

    std::unique_ptr<ILayoutComponent> createBox(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<BoxComponent>(ctx, node);
    }
  } // namespace

  void registerBoxComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "box",
                                .displayName = "Box",
                                .category = ComponentCategory::Container,
                                .props = {{.name = "orientation",
                                           .kind = PropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"vertical"},
                                           .enumValues = {"vertical", "horizontal"}},
                                          {.name = "spacing",
                                           .kind = PropertyKind::Int,
                                           .label = "Spacing",
                                           .defaultValue = LayoutValue{static_cast<std::int64_t>(0)}},
                                          {.name = "homogeneous",
                                           .kind = PropertyKind::Bool,
                                           .label = "Homogeneous",
                                           .defaultValue = LayoutValue{false}}},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = std::nullopt},
                               createBox);
  }
} // namespace ao::gtk::layout
