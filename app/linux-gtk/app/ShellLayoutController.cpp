// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/ShellLayoutController.h"
#include "layout/LayoutConstants.h"
#include "layout/document/LayoutYaml.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/LayoutRuntime.h"
#include <ao/utility/Log.h>
#include <gtkmm/cssprovider.h>
#include <runtime/ConfigStore.h>

namespace ao::gtk
{
  ShellLayoutController::ShellLayoutController(rt::AppSession& session, Gtk::Window& parentWindow)
    : _registry{}
    , _context{.registry = _registry, .session = session, .parentWindow = parentWindow, .onNodeMoved = {}}
    , _host{_registry}
  {
    layout::LayoutRuntime::registerStandardComponents(_registry);
  }

  void ShellLayoutController::attachToWindow()
  {
    _context.parentWindow.set_child(_host);
    setupCss();
  }

  void ShellLayoutController::loadLayout(rt::ConfigStore& configStore)
  {
    auto doc = layout::createDefaultLayout();
    if (auto const res = configStore.load("linuxGtkLayout", doc); !res && res.error().code != Error::Code::NotFound)
    {
      APP_LOG_DEBUG("Failed to load layout from config: {}", res.error().message);
    }
    _activeLayout = doc;
    _host.setLayout(_context, _activeLayout);
  }

  void ShellLayoutController::saveLayout(rt::ConfigStore& configStore) const
  {
    configStore.save("linuxGtkLayout", _activeLayout);
  }

  void ShellLayoutController::openEditor(rt::ConfigStore& configStore)
  {
    auto const dialog = std::make_shared<layout::editor::LayoutEditorDialog>(
      dynamic_cast<Gtk::Window&>(_context.parentWindow), _registry, _activeLayout);
    auto* const dialogPtr = dialog.get();

    _context.editMode = true;
    _context.onNodeMoved = [dialogPtr](std::string const& nodeId, int posX, int posY)
    { dialogPtr->updateNodePosition(nodeId, posX, posY); };

    _host.setLayout(_context, _activeLayout);

    dialogPtr->signalApplyPreview().connect([this](layout::LayoutDocument const& doc)
                                            { _host.setLayout(_context, doc); });

    dialogPtr->signal_response().connect(
      [this, sharedDialog = dialog, &configStore](int responseId)
      {
        _context.editMode = false;
        _context.onNodeMoved = nullptr;

        if (responseId == Gtk::ResponseType::OK)
        {
          _activeLayout = sharedDialog->document();
          _host.setLayout(_context, _activeLayout);
          configStore.save("linuxGtkLayout", _activeLayout);
        }
        else if (responseId == Gtk::ResponseType::CANCEL)
        {
          _host.setLayout(_context, _activeLayout);
        }

        sharedDialog->close();
      });

    dialogPtr->present();
  }

  void ShellLayoutController::setupCss()
  {
    auto const cssProvider = Gtk::CssProvider::create();
    cssProvider->load_from_data(".inspector-handle {"
                                "  min-width: 14px;"
                                "  padding: 0;"
                                "  margin: 0;"
                                "  border: none;"
                                "  border-radius: 0;"
                                "  background: transparent;"
                                "  transition: background 0.2s;"
                                "}"
                                ".inspector-handle:hover {"
                                "  background: alpha(currentColor, 0.08);"
                                "}"
                                ".inspector-handle image {"
                                "  opacity: 0.4;"
                                "  transition: opacity 0.2s;"
                                "}"
                                ".inspector-handle:hover image {"
                                "  opacity: 1.0;"
                                "}"
                                ".tags-section {"
                                "  margin-top: 4px;"
                                "}"
                                ".tag-chip {"
                                "  border-radius: 100px;"
                                "  padding: 4px 10px;"
                                "  font-size: 0.85rem;"
                                "  font-weight: 500;"
                                "  transition: all 0.2s ease;"
                                "}"
                                "togglebutton.tag-chip {"
                                "  background: alpha(currentColor, 0.05);"
                                "  border: 1px solid transparent;"
                                "  color: alpha(currentColor, 0.7);"
                                "}"
                                "togglebutton.tag-chip:checked {"
                                "  background: alpha(currentColor, 0.15);"
                                "  color: currentColor;"
                                "  border-color: alpha(currentColor, 0.1);"
                                "}"
                                "togglebutton.tag-chip:hover {"
                                "  background: alpha(currentColor, 0.2);"
                                "}"
                                ".tag-remove-button {"
                                "  min-width: 18px;"
                                "  min-height: 18px;"
                                "  padding: 0;"
                                "  margin-left: 4px;"
                                "  border-radius: 100px;"
                                "  background: transparent;"
                                "  border: none;"
                                "  opacity: 0.4;"
                                "  transition: opacity 0.2s;"
                                "}"
                                ".tag-remove-button:hover {"
                                "  opacity: 1.0;"
                                "  background: alpha(@error_color, 0.1);"
                                "}"
                                ".tags-entry {"
                                "  background: alpha(currentColor, 0.05);"
                                "  border: 1px solid transparent;"
                                "  border-radius: 8px;"
                                "  padding: 6px 12px;"
                                "  margin-top: 8px;"
                                "  transition: all 0.2s;"
                                "  font-size: 0.9rem;"
                                "}"
                                ".tags-entry:focus {"
                                "  border-color: alpha(@accent_color, 0.5);"
                                "  background: alpha(currentColor, 0.08);"
                                "  box-shadow: none;"
                                "}");
    Gtk::StyleContext::add_provider_for_display(
      Gdk::Display::get_default(), cssProvider, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
} // namespace ao::gtk
