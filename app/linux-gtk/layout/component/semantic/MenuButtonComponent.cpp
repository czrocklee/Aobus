// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "common/AccessibleLabel.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/widget.h>

#include <memory>
#include <string>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief menuButton
     */
    class MenuButtonComponent final : public LayoutComponent
    {
    public:
      MenuButtonComponent(Glib::RefPtr<Gio::MenuModel> const& menuModelPtr,
                          i18n::MessageCatalog const& textCatalog,
                          LayoutNode const& node)
      {
        // The vocabulary lets a document name the button; without one it is
        // still the application menu, and still has to describe itself.
        auto const fallbackLabel = gtkText(textCatalog, i18n::MessageId::GtkShellApplicationMenu);
        setTooltipAndAccessibleLabel(_button, node.propertyOr<std::string>(kTextProp, fallbackLabel));

        if (auto const icon = node.propertyOr<std::string>("icon", ""); !icon.empty())
        {
          _button.set_icon_name(icon);
        }

        auto const style = node.propertyOr<std::string>("style", "flat");

        if (style == "flat")
        {
          _button.set_has_frame(false);
        }

        if (menuModelPtr)
        {
          _button.set_menu_model(menuModelPtr);
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::MenuButton _button;
    };
  } // namespace

  void registerMenuButtonComponent(ComponentRegistry& registry,
                                   Glib::RefPtr<Gio::MenuModel> const& menuModelPtr,
                                   i18n::MessageCatalog const& textCatalog)
  {
    registry.registerSharedComponent(
      "menuButton",
      {.properties = {{.name = "icon", .kind = PropertyKind::String, .label = "Icon (Symbolic)"},
                      {.name = "style",
                       .kind = PropertyKind::Enum,
                       .label = "Style",
                       .defaultValue = LayoutValue{"flat"},
                       .enumValues = {"flat", "raised"}}}},
      [menuModelPtr, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& node)
      { return std::make_unique<MenuButtonComponent>(menuModelPtr, textCatalog, node); });
  }
} // namespace ao::gtk::layout
