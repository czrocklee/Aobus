// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/paned.h>
#include <gtkmm/widget.h>

#include <cstdint>
#include <memory>
#include <string>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief A split container component (Gtk::Paned).
     */
    class SplitComponent final : public ILayoutComponent
    {
    public:
      SplitComponent(LayoutContext& ctx, LayoutNode const& node)
      {
        if (node.children.size() != 2)
        {
          _errorPtr = std::make_unique<Gtk::Label>();
          _errorPtr->set_markup(
            "<span foreground='red'><b>[Layout Error]</b> split requires exactly 2 children</span>");
          _errorPtr->add_css_class("ao-layout-error");
          return;
        }

        auto orientation = Gtk::Orientation::VERTICAL;

        if (node.getProp<std::string>("orientation", "") == "horizontal")
        {
          orientation = Gtk::Orientation::HORIZONTAL;
        }

        _paned.set_orientation(orientation);

        _startChildPtr = ctx.registry.create(ctx, node.children[0]);
        _paned.set_start_child(_startChildPtr->widget());

        _endChildPtr = ctx.registry.create(ctx, node.children[1]);
        _paned.set_end_child(_endChildPtr->widget());

        _paned.set_resize_start_child(node.getProp<bool>("resizeStart", true));
        _paned.set_shrink_start_child(node.getProp<bool>("shrinkStart", false));
        _paned.set_resize_end_child(node.getProp<bool>("resizeEnd", true));
        _paned.set_shrink_end_child(node.getProp<bool>("shrinkEnd", false));

        if (auto const it = node.props.find("position"); it != node.props.end())
        {
          _paned.set_position(static_cast<std::int32_t>(it->second.asInt()));
        }
      }

      Gtk::Widget& widget() override
      {
        return (_errorPtr != nullptr) ? static_cast<Gtk::Widget&>(*_errorPtr) : static_cast<Gtk::Widget&>(_paned);
      }

    private:
      Gtk::Paned _paned;
      std::unique_ptr<Gtk::Label> _errorPtr;
      std::unique_ptr<ILayoutComponent> _startChildPtr;
      std::unique_ptr<ILayoutComponent> _endChildPtr;
    };

    std::unique_ptr<ILayoutComponent> createSplit(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SplitComponent>(ctx, node);
    }
  } // namespace

  void registerSplitComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "split",
       .displayName = "Split Pane",
       .category = "Containers",
       .container = true,
       .props =
         {{.name = "orientation",
           .kind = PropertyKind::Enum,
           .label = "Orientation",
           .defaultValue = LayoutValue{"vertical"},
           .enumValues = {"vertical", "horizontal"}},
          {.name = "position",
           .kind = PropertyKind::Int,
           .label = "Position",
           .defaultValue = LayoutValue{static_cast<std::int64_t>(-1)}},
          {.name = "resizeStart",
           .kind = PropertyKind::Bool,
           .label = "Resize Start",
           .defaultValue = LayoutValue{true}},
          {.name = "shrinkStart",
           .kind = PropertyKind::Bool,
           .label = "Shrink Start",
           .defaultValue = LayoutValue{false}},
          {.name = "resizeEnd", .kind = PropertyKind::Bool, .label = "Resize End", .defaultValue = LayoutValue{true}},
          {.name = "shrinkEnd", .kind = PropertyKind::Bool, .label = "Shrink End", .defaultValue = LayoutValue{false}}},
       .layoutProps = {},
       .minChildren = 2,
       .optMaxChildren = 2},
      createSplit);
  }
} // namespace ao::gtk::layout
