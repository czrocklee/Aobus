// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "i18n/GtkText.h"
#include "layout/component/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <pangomm/layout.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief status.message
     */
    class StatusMessageLabelComponent final : public LayoutComponent
    {
    public:
      explicit StatusMessageLabelComponent(i18n::MessageCatalog const& textCatalog)
      {
        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.set_halign(Gtk::Align::START);
        _label.set_text(gtkText(textCatalog, i18n::MessageId::GtkStatusReady));
      }

      Gtk::Widget& widget() override { return _label; }

    private:
      Gtk::Label _label;
    };
  } // namespace

  void registerStatusMessageLabelComponent(ComponentRegistry& registry, i18n::MessageCatalog const& textCatalog)
  {
    registry.registerSharedComponent("status.message",
                                     [textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
                                     { return std::make_unique<StatusMessageLabelComponent>(textCatalog); });
  }
} // namespace ao::gtk::layout
