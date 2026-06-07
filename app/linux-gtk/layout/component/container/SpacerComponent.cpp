// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/document/LayoutNode.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"

#include <gtkmm/box.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  namespace
  {
    /**
     * @brief A simple spacer component.
     */
    class SpacerComponent final : public ILayoutComponent
    {
    public:
      SpacerComponent(LayoutContext& /*ctx*/, LayoutNode const& /*node*/) {}

      Gtk::Widget& widget() override { return _box; }

    private:
      Gtk::Box _box;
    };

    std::unique_ptr<ILayoutComponent> createSpacer(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SpacerComponent>(ctx, node);
    }
  } // namespace

  void registerSpacerComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "spacer",
                                .displayName = "Spacer",
                                .category = "Containers",
                                .container = false,
                                .props = {},
                                .layoutProps = {},
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSpacer);
  }
} // namespace ao::gtk::layout
