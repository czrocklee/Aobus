// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "ContainerComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/box.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief A simple spacer component.
     */
    class SpacerComponent final : public LayoutComponent
    {
    public:
      SpacerComponent(LayoutBuildContext& /*ctx*/, LayoutNode const& /*node*/) {}

      Gtk::Widget& widget() override { return _box; }

    private:
      Gtk::Box _box;
    };

    std::unique_ptr<LayoutComponent> createSpacer(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SpacerComponent>(ctx, node);
    }
  } // namespace

  void registerSpacerComponent(ComponentRegistry& registry)
  {
    registry.registerComponent({.type = "spacer",
                                .displayName = "Spacer",
                                .category = LayoutComponentCategory::Container,
                                .minChildren = 0,
                                .optMaxChildren = 0},
                               createSpacer);
  }
} // namespace ao::gtk::layout
