// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/layout/component/SharedLayoutComponentType.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/popovermenubar.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief app.menuBar
     */
    class MenuBarComponent final : public LayoutComponent
    {
    public:
      explicit MenuBarComponent(Glib::RefPtr<Gio::MenuModel> const& menuModelPtr)
      {
        if (menuModelPtr)
        {
          _menuBar.set_menu_model(menuModelPtr);
        }
      }

      Gtk::Widget& widget() override { return _menuBar; }

    private:
      Gtk::PopoverMenuBar _menuBar;
    };
  } // namespace

  void registerMenuBarComponent(ComponentRegistry& registry, Glib::RefPtr<Gio::MenuModel> const& menuModelPtr)
  {
    registry.registerComponent(sharedComponentDescriptor(SharedLayoutComponentType::MenuBar),
                               [menuModelPtr](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
                               { return std::make_unique<MenuBarComponent>(menuModelPtr); });
  }
} // namespace ao::gtk::layout
