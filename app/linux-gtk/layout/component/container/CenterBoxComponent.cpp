// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/centerbox.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief A center box container component (Gtk::CenterBox).
     */
    class CenterBoxComponent final : public LayoutComponent
    {
    public:
      CenterBoxComponent(LayoutBuildContext& ctx, LayoutNode const& node)
      {
        auto orientation = Gtk::Orientation::HORIZONTAL;

        if (node.propertyOr<std::string>("orientation", "") == "vertical")
        {
          orientation = Gtk::Orientation::VERTICAL;
        }

        _centerBox.set_orientation(orientation);

        for (auto const& childNode : node.children)
        {
          auto childPtr = ctx.registry.create(ctx, childNode);

          auto const slot = childNode.layoutOr<std::string>("slot", "");

          if (slot == "start")
          {
            _centerBox.set_start_widget(childPtr->widget());
            _startChildPtr = std::move(childPtr);
          }
          else if (slot == "center")
          {
            _centerBox.set_center_widget(childPtr->widget());
            _centerChildPtr = std::move(childPtr);
          }
          else if (slot == "end")
          {
            _centerBox.set_end_widget(childPtr->widget());
            _endChildPtr = std::move(childPtr);
          }
          else
          {
            _overflowChildren.push_back(std::move(childPtr));
          }
        }
      }

      Gtk::Widget& widget() override { return _centerBox; }

    private:
      Gtk::CenterBox _centerBox;
      std::unique_ptr<LayoutComponent> _startChildPtr;
      std::unique_ptr<LayoutComponent> _centerChildPtr;
      std::unique_ptr<LayoutComponent> _endChildPtr;
      std::vector<std::unique_ptr<LayoutComponent>> _overflowChildren;
    };

    std::unique_ptr<LayoutComponent> createCenterBox(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<CenterBoxComponent>(ctx, node);
    }
  } // namespace

  void registerCenterBoxComponent(ComponentRegistry& registry)
  {
    constexpr std::size_t kCenterBoxMaxChildren = 3;
    registry.registerComponent({.type = "centerBox",
                                .displayName = "Center Box",
                                .category = LayoutComponentCategory::Container,
                                .props = {{.name = "orientation",
                                           .kind = LayoutPropertyKind::Enum,
                                           .label = "Orientation",
                                           .defaultValue = LayoutValue{"horizontal"},
                                           .enumValues = {"horizontal", "vertical"}}},
                                .layoutProps = {{.name = "slot",
                                                 .kind = LayoutPropertyKind::Enum,
                                                 .label = "Slot",
                                                 .defaultValue = LayoutValue{""},
                                                 .enumValues = {"", "start", "center", "end"}}},
                                .minChildren = 0,
                                .optMaxChildren = kCenterBoxMaxChildren},
                               createCenterBox);
  }
} // namespace ao::gtk::layout
