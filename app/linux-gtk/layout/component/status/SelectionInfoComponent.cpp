// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/SelectionInfoLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    class SelectionInfoComponent final : public LayoutComponent
    {
    public:
      SelectionInfoComponent(LayoutBuildContext& ctx, LayoutNode const& /*node*/)
        : _widget{ctx.runtime.views()}
      {
        _widget.widget().add_css_class("ao-selection-info-modern");
      }

      Gtk::Widget& widget() override { return _widget.widget(); }

    private:
      SelectionInfoLabel _widget;
    };

    std::unique_ptr<LayoutComponent> createSelectionInfo(LayoutBuildContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<SelectionInfoComponent>(ctx, node);
    }
  } // namespace

  void registerSelectionInfoComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.selectionInfo", .displayName = "Selection Info", .category = LayoutComponentCategory::Status},
      createSelectionInfo);
  }
} // namespace ao::gtk::layout
