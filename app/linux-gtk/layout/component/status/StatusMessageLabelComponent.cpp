// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatusComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/ILayoutComponent.h"
#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ComponentCatalog.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel::layout;
  namespace
  {
    /**
     * @brief status.messageLabel
     */
    class StatusMessageLabelComponent final : public ILayoutComponent
    {
    public:
      StatusMessageLabelComponent(LayoutContext& /*ctx*/, LayoutNode const& /*node*/)
      {
        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.set_halign(Gtk::Align::START);
        _label.set_text("Aobus Ready");
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };

    std::unique_ptr<ILayoutComponent> createStatusMessageLabel(LayoutContext& ctx, LayoutNode const& node)
    {
      return std::make_unique<StatusMessageLabelComponent>(ctx, node);
    }
  } // namespace

  void registerStatusMessageLabelComponent(ComponentRegistry& registry)
  {
    registry.registerComponent(
      {.type = "status.messageLabel", .displayName = "Status Message (Basic)", .category = ComponentCategory::Status},
      createStatusMessageLabel);
  }
} // namespace ao::gtk::layout
