// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "SemanticComponentRegistrations.h"
#include "common/AccessibleLabel.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "portal/ImportExportActions.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <gtkmm/button.h>
#include <gtkmm/widget.h>

#include <memory>

namespace ao::gtk::layout
{
  using namespace uimodel;
  namespace
  {
    /**
     * @brief library.openLibraryButton
     */
    class OpenLibraryButton final : public LayoutComponent
    {
    public:
      OpenLibraryButton(portal::ImportExportActions* actions, i18n::MessageCatalog const& textCatalog)
      {
        auto const label = gtkText(textCatalog, i18n::MessageId::GtkShellOpenLibrary);
        _button.set_icon_name("folder-open-symbolic");
        setTooltipAndAccessibleLabel(_button, label);

        if (actions != nullptr)
        {
          _button.signal_clicked().connect([actions] { actions->openLibrary(); });
        }
        else
        {
          _button.set_sensitive(false);
        }
      }

      Gtk::Widget& widget() override { return _button; }

    private:
      Gtk::Button _button;
    };
  } // namespace

  void registerOpenLibraryButtonComponent(ComponentRegistry& registry,
                                          portal::ImportExportActions* importExportActions,
                                          i18n::MessageCatalog const& textCatalog)
  {
    registry.registerComponent(
      {.id = "library.openLibraryButton",
       .displayName = "Open Library Button",
       .category = ComponentCategory::Library,
       .minChildren = 0,
       .optMaxChildren = 0},
      [importExportActions, textCatalog](LayoutBuildContext const& /*ctx*/, LayoutNode const& /*node*/)
      { return std::make_unique<OpenLibraryButton>(importExportActions, textCatalog); });
  }
} // namespace ao::gtk::layout
